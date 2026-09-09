#include "lob/book/order_book.hpp"

#include <stdexcept>

namespace lob {

Order* OrderBook::addOrder(OrderId orderId, Price price, Quantity quantity,
						   Timestamp timestamp, OrderSide orderSide,
						   OrderType orderType, SequenceNumber sequenceNumber) {
	if (orderType == OrderType::MARKET) {
		throw std::invalid_argument("OrderBook accepts resting limit orders only");
	}
	if (orderIndex.findOrder(orderId) != nullptr) {
		throw std::logic_error("Order with the same OrderId already exists in the book");
	}

	Order* order = orderPool.allocate(orderId, price, quantity, timestamp,
									  orderSide, orderType, sequenceNumber);
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

bool OrderBook::cancelOrder(const OrderId& orderId) {
	Order* order = orderIndex.findOrder(orderId);
	if (order == nullptr) {
		return false;
	}

	PriceLevels& levels = order->isBuy() ? bids : asks;
	auto levelIterator = levels.find(order->getPrice());
	if (levelIterator == levels.end()) {
		throw std::logic_error("Order price level not found");
	}

	PriceLevel& level = levelIterator->second;
	level.removeOrder(order);
	if (level.isEmpty()) {
		levels.erase(levelIterator);
	}
	orderIndex.removeOrder(order);
	orderPool.release(order);
	return true;
}

Order* OrderBook::findOrder(const OrderId& orderId) {
	return orderIndex.findOrder(orderId);
}

const Order* OrderBook::findOrder(const OrderId& orderId) const {
	return orderIndex.findOrder(orderId);
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
