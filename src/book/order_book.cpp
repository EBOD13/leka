#include "lob/book/order_book.hpp"

#include <stdexcept>

namespace lob {

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
	if (orderIndex.findOrder(orderId) != nullptr) {
		throw std::logic_error("Order with the same OrderId already exists in the book");
	}

	Order* order = orderPool.allocate(orderId, price, originalQuantity,
									  remainingQuantity, timestamp, orderSide,
									  orderType, sequenceNumber);
	bool levelCreated = false;
	bool addedToLevel = false;
	PriceLevels* levels = nullptr;
	PriceLevels::iterator levelIterator;

	try {
		if (!order->isValid()) {
			throw std::invalid_argument("Cannot add an invalid order");
		}

		levels = order->isBuy() ? &bids : &asks;
		auto result = levels->try_emplace(order->getPrice(), order->getPrice());
		levelIterator = result.first;
		levelCreated = result.second;
		levelIterator->second.addOrder(order);
		addedToLevel = true;

		orderIndex.addOrder(order);
	} catch (...) {
		if (addedToLevel) {
			levelIterator->second.removeOrder(order);
		}
		if (levelCreated && levelIterator->second.isEmpty()) {
			levels->erase(levelIterator);
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
 * @details A same-price decrease updates the existing FIFO node. All other
 * effective changes use the normal remove-and-add path so the order joins the
 * tail of its replacement price level.
 */
bool OrderBook::modifyOrder(const OrderId& orderId, Price newPrice,
							Quantity newQuantity, SequenceNumber newSequenceNumber) {
	Order* order = orderIndex.findOrder(orderId);
	if (order == nullptr) {
		return false;
	}
	if (!newPrice.isValid() || !newQuantity.isValid()) {
		throw std::invalid_argument("Invalid modification value");
	}

	const bool samePrice =
		order->getPrice().getPrice() == newPrice.getPrice();
	const bool quantityDecreased = newQuantity < order->getRemainingQuantity();
	if (samePrice && newQuantity.getQuantity() ==
			order->getRemainingQuantity().getQuantity()) {
		return true;
	}
	if (samePrice && quantityDecreased) {
		const Quantity reduction{
			order->getRemainingQuantity().getQuantity() - newQuantity.getQuantity()};
		order->setRemainingQuantity(newQuantity);
		order->getPriceLevel()->reduceTotalQuantity(reduction);
		return true;
	}
	if (!newSequenceNumber.isValid()) {
		throw std::invalid_argument("Priority-reset modification requires a sequence number");
	}

	const OrderId id = order->getOrderId();
	const Timestamp timestamp = order->getTimestamp();
	const OrderSide side = order->getOrderSide();
	const OrderType type = order->getOrderType();
	removeOrder(order);
	addOrder(id, newPrice, newQuantity, timestamp, side, type, newSequenceNumber);
	return true;
}

/**
 * @details This is the single removal path used by cancellation and matching.
 * It removes non-owning references while the Order is alive, then destroys and
 * recycles the object through OrderPool.
 */
void OrderBook::removeOrder(Order* order) {
	if (order == nullptr) {
		throw std::invalid_argument("Cannot remove a null order");
	}
	if (orderIndex.findOrder(order->getOrderId()) != order) {
		throw std::logic_error("Order is not indexed in this book");
	}

	PriceLevels& levels = order->isBuy() ? bids : asks;
	auto levelIterator = levels.find(order->getPrice());
	if (levelIterator == levels.end()) {
		throw std::logic_error("Order price level not found");
	}

	PriceLevel& level = levelIterator->second;
	if (order->getPriceLevel() != &level) {
		throw std::logic_error("Order does not belong to its indexed price level");
	}
	level.removeOrder(order);
	if (level.isEmpty()) {
		levels.erase(levelIterator);
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
	if (bids.empty()) {
		return nullptr;
	}
	return &bids.rbegin()->second;
}

PriceLevel* OrderBook::getBestAsk() {
	if (asks.empty()) {
		return nullptr;
	}
	return &asks.begin()->second;
}

const PriceLevel* OrderBook::getBestBid() const {
	if (bids.empty()) {
		return nullptr;
	}
	return &bids.rbegin()->second;
}

const PriceLevel* OrderBook::getBestAsk() const {
	if (asks.empty()) {
		return nullptr;
	}
	return &asks.begin()->second;
}

} // namespace lob
