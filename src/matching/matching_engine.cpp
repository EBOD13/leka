#include "lob/matching/matching_engine.hpp"

#include <algorithm>
#include <stdexcept>

namespace lob {

/** @file Implements event dispatch and price/time matching behavior. */

MatchingEngine::MatchingEngine(OrderBook& orderBook)
	: orderBook(orderBook) {}

/** Convenience wrapper: identical allocation behavior to the pre-buffer API. */
std::vector<Execution> MatchingEngine::processEvent(const OrderEvent& event) {
	std::vector<Execution> out;
	processEvent(event, out);
	return out;
}

/** Dispatches by event type so only the active payload is interpreted. */
std::size_t MatchingEngine::processEvent(const OrderEvent& event, std::vector<Execution>& out) {
	out.clear();
	switch (event.getEventType()) {
		case OrderEventType::NEW: {
			const NewOrder& order = event.getNewOrder();
			matchOrder(order.orderId, order.price, order.quantity,
					  order.timestamp, order.orderSide, order.orderType, out);
			return out.size();
		}
		case OrderEventType::CANCEL:
			if (!event.getCancelOrder().orderId.isValid()) {
				throw std::invalid_argument("Invalid cancel order ID");
			}
			orderBook.cancelOrder(event.getCancelOrder().orderId);
			return 0;
		case OrderEventType::REDUCE: {
			const ReduceOrder& reduction = event.getReduceOrder();
			if (!reduction.orderId.isValid()) {
				throw std::invalid_argument("Invalid reduce order ID");
			}
			// A reduction never changes price and never forfeits priority, so
			// there is no decision to make here and no sequence number to
			// allocate. The book resolves the order once and mutates in place.
			orderBook.reduceOrder(reduction.orderId, reduction.newQuantity);
			return 0;
		}
	}
	throw std::logic_error("Unknown order event type");
}

/** Forwards the event-oriented overload to processEvent(). */
std::vector<Execution> MatchingEngine::processOrder(const OrderEvent& event) {
	return processEvent(event);
}

/** Convenience wrapper: identical allocation behavior to the pre-buffer API. */
std::vector<Execution> MatchingEngine::processOrder(
	OrderId orderId, Price price, Quantity quantity, Timestamp timestamp,
	OrderSide orderSide, OrderType orderType) {
	std::vector<Execution> out;
	processOrder(orderId, price, quantity, timestamp, orderSide, orderType, out);
	return out;
}

std::size_t MatchingEngine::processOrder(
	OrderId orderId, Price price, Quantity quantity, Timestamp timestamp,
	OrderSide orderSide, OrderType orderType, std::vector<Execution>& out) {
	out.clear();
	matchOrder(orderId, price, quantity, timestamp, orderSide, orderType, out);
	return out.size();
}

/**
 * @details
 * Matching is performed without allocating an incoming Order. The incoming
 * quantity remains local until a partially filled limit order must rest in the
 * book. Resting orders are always consumed from the best opposing level's FIFO
 * head, and filled resting orders are removed through OrderBook so its index,
 * price level, and pool remain synchronized.
 *
 * This is the single implementation of the matching algorithm; every public
 * entry point in this file funnels into it. It never clears @c out, so a
 * caller reusing one buffer across many calls controls exactly when that
 * buffer's contents are discarded.
 */
void MatchingEngine::matchOrder(
	OrderId orderId, Price price, Quantity quantity, Timestamp timestamp,
	OrderSide orderSide, OrderType orderType, std::vector<Execution>& out) {
	if (!orderId.isValid() || !quantity.isValid() || !timestamp.isValid()) {
		throw std::invalid_argument("Invalid incoming order value");
	}
	if (orderSide != OrderSide::BUY && orderSide != OrderSide::SELL) {
		throw std::invalid_argument("Invalid incoming order side");
	}
	if (orderType != OrderType::LIMIT && orderType != OrderType::MARKET) {
		throw std::invalid_argument("Invalid incoming order type");
	}
	if (orderType == OrderType::LIMIT && !price.isValid()) {
		throw std::invalid_argument("Limit order requires a valid price");
	}
	if (orderType == OrderType::MARKET && price.isValid()) {
		throw std::invalid_argument("Market order must not specify a price");
	}
	if (orderBook.findOrder(orderId) != nullptr) {
		throw std::logic_error("Order with the same OrderId already exists");
	}

	const SequenceNumber sequenceNumber = sequenceNumberGenerator.generate();
	const Quantity originalQuantity = quantity;
	std::uint64_t remaining = quantity.getQuantity();

	// Always inspect the best opposing level first; its FIFO head determines
	// both price priority and time priority for the next execution.
	while (remaining > 0) {
		PriceLevel* level = orderSide == OrderSide::BUY
			? orderBook.getBestAsk()
			: orderBook.getBestBid();
		if (level == nullptr) {
			break;
		}

		const Price bestPrice = level->getPrice();
		if (orderType == OrderType::LIMIT) {
			if (orderSide == OrderSide::BUY && price < bestPrice) {
				break;
			}
			if (orderSide == OrderSide::SELL && price > bestPrice) {
				break;
			}
		}

		Order* restingOrder = level->getHeadOrder();
		if (restingOrder == nullptr) {
			throw std::logic_error("Non-empty price level has no head order");
		}

		const std::uint64_t restingRemaining =
			restingOrder->getRemainingQuantity().getQuantity();
		const std::uint64_t executionValue =
			std::min(remaining, restingRemaining);
		const Quantity executionQuantity{executionValue};

		out.emplace_back(orderId, restingOrder->getOrderId(),
						 restingOrder->getPrice(), executionQuantity);

		remaining -= executionValue;
		restingOrder->reduceRemainingQuantity(executionQuantity);

		// Full fills are removed through OrderBook so every index stays in sync.
		// Partial fills update the level aggregate separately because removal
		// would otherwise subtract the order's entire remaining quantity again.
		if (restingOrder->isFullyFilled()) {
			orderBook.removeOrder(restingOrder);
		} else {
			level->reduceTotalQuantity(executionQuantity);
		}
	}

	if (remaining > 0 && orderType == OrderType::LIMIT) {
		orderBook.addRestingRemainder(
			orderId, price, originalQuantity, Quantity{remaining}, timestamp,
			orderSide, OrderType::LIMIT, sequenceNumber);
	}
}

} // namespace lob
