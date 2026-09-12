#include "lob/book/order_book.hpp"

#include <stdexcept>

namespace lob {

/** @details See the header for why 65536 levels of tick 1 starting at 1 are adequate defaults for tests and examples only. */
OrderBook::OrderBook() : OrderBook(Price{1}, Price{1}, DefaultLevelCount) {}

OrderBook::OrderBook(Price minPrice, Price tickSize, std::size_t levelCount)
	: bids(minPrice, tickSize, levelCount, /*descending=*/true),
	  asks(minPrice, tickSize, levelCount, /*descending=*/false) {}

/**
 * @details A new order starts with equal original and remaining quantities.
 * The shared insertion helper provides the same transactional guarantees as
 * remainder insertion while keeping the public API concise.
 */
Order* OrderBook::addOrder(OrderId orderId, Price price, Quantity quantity,
						   Timestamp timestamp, OrderSide orderSide,
						   OrderType orderType, SequenceNumber sequenceNumber) {
	return addOrderWithQuantities(orderId, price, quantity, quantity, timestamp,
								  orderSide, orderType, sequenceNumber);
}

/**
 * @details The original quantity is preserved for audit and lifecycle
 * semantics; only the remaining quantity is placed into the level aggregate.
 */
Order* OrderBook::addRestingRemainder(
		OrderId orderId, Price price, Quantity originalQuantity,
		Quantity remainingQuantity, Timestamp timestamp, OrderSide orderSide,
		OrderType orderType, SequenceNumber sequenceNumber) {
	return addOrderWithQuantities(orderId, price, originalQuantity,
								  remainingQuantity, timestamp, orderSide,
								  orderType, sequenceNumber);
}

/**
 * @details Insertion is committed in this order: construct the Order, link it
 * into the PriceLevel, then add the pointer to OrderIndex. On failure, the
 * level link and any newly created level are removed before pool release.
 */
Order* OrderBook::addOrderWithQuantities(
		OrderId orderId, Price price, Quantity originalQuantity,
		Quantity remainingQuantity, Timestamp timestamp, OrderSide orderSide,
		OrderType orderType, SequenceNumber sequenceNumber) {
	if (orderType != OrderType::LIMIT) {
		throw std::invalid_argument("OrderBook accepts resting limit orders only");
	}
	if (!originalQuantity.isValid() || !remainingQuantity.isValid() ||
		remainingQuantity > originalQuantity) {
		throw std::invalid_argument("Invalid original or remaining quantity");
	}
	// Duplicate IDs are detected by the index insertion below, which already
	// hashes the key. Probing for the ID here first would hash it a second
	// time on every accepted insert to catch a case the rollback handles.
	Order* order = orderPool.allocate(orderId, price, originalQuantity,
									  remainingQuantity, timestamp, orderSide,
									  orderType, sequenceNumber);
	bool addedToLevel = false;
	PriceLadder* ladder = nullptr;
	PriceLevel* level = nullptr;

	try {
		if (!order->isValid()) {
			throw std::invalid_argument("Cannot add an invalid order");
		}

		// The level always exists; there is nothing to create and nothing
		// that can fail here except an out-of-range or misaligned price.
		ladder = order->isBuy() ? &bids : &asks;
		level = &ladder->levelAt(order->getPrice());
		const bool wasEmpty = level->isEmpty();
		level->addOrder(order);
		addedToLevel = true;
		if (wasEmpty) {
			ladder->markOccupied(order->getPrice());
		}

		orderIndex.addOrder(order);
	} catch (...) {
		if (addedToLevel) {
			level->removeOrder(order);
			if (level->isEmpty()) {
				ladder->markEmpty(order->getPrice());
			}
		}
		orderPool.release(order);
		throw;
	}

	return order;
}

/** @details Looks up the order, then delegates all structural cleanup to removeOrder(). */
bool OrderBook::cancelOrder(const OrderId& orderId) {
	Order* order = orderIndex.findOrder(orderId);
	if (order == nullptr) {
		return false;
	}
	removeOrder(order);
	return true;
}

/**
 * @details The order is resolved once and mutated where it lies. Because the
 * price is unchanged the order cannot move between levels, so no relinking,
 * no level lookup, and no sequence number are involved.
 */
bool OrderBook::reduceOrder(const OrderId& orderId, Quantity newQuantity) {
	Order* order = orderIndex.findOrder(orderId);
	if (order == nullptr) {
		return false;
	}

	const std::uint64_t remaining = order->getRemainingQuantity().getQuantity();
	const std::uint64_t target = newQuantity.getQuantity();
	if (target == 0 || target > remaining) {
		throw std::invalid_argument(
			"Reduction requires a nonzero quantity no greater than the remaining quantity");
	}
	if (target == remaining) {
		return true;
	}

	order->setRemainingQuantity(newQuantity);
	order->getPriceLevel()->reduceTotalQuantity(Quantity{remaining - target});
	return true;
}

/**
 * @details This is the single removal path used by cancellation and matching.
 * It removes non-owning references while the Order is alive, then destroys and
 * recycles the object through OrderPool.
 *
 * Order already carries a direct pointer to its PriceLevel, set when it was
 * inserted, so removal uses that pointer instead of independently
 * re-deriving the level from price through the ladder. PriceLevel::removeOrder
 * still checks that the order actually belongs to the level it names, which
 * is the same identity check a map-based re-lookup would have produced; the
 * ladder-based lookup here would only have been useful for detecting a
 * corrupted Order::priceLevel pointer, at the cost of a lookup on every
 * removal to guard against a case OrderPool's own invariants already rule out.
 */
void OrderBook::removeOrder(Order* order) {
	if (order == nullptr) {
		throw std::invalid_argument("Cannot remove a null order");
	}
	if (orderIndex.findOrder(order->getOrderId()) != order) {
		throw std::logic_error("Order is not indexed in this book");
	}

	PriceLevel* level = order->getPriceLevel();
	if (level == nullptr) {
		throw std::logic_error("Order is not linked to a price level");
	}
	PriceLadder& ladder = order->isBuy() ? bids : asks;
	const Price price = order->getPrice();

	level->removeOrder(order);
	if (level->isEmpty()) {
		ladder.markEmpty(price);
	}
	orderIndex.removeOrder(order);
	orderPool.release(order);
}

Order* OrderBook::findOrder(const OrderId& orderId) {
	return orderIndex.findOrder(orderId);
}

const Order* OrderBook::findOrder(const OrderId& orderId) const {
	return orderIndex.findOrder(orderId);
}

PriceLevel* OrderBook::getBestBid() {
	return bids.best();
}

PriceLevel* OrderBook::getBestAsk() {
	return asks.best();
}

const PriceLevel* OrderBook::getBestBid() const {
	return bids.best();
}

const PriceLevel* OrderBook::getBestAsk() const {
	return asks.best();
}

void OrderBook::reserveOrderCapacity(std::size_t orderCount) {
	orderPool.reserve(orderCount);
	orderIndex.reserve(orderCount);
}

} // namespace lob
