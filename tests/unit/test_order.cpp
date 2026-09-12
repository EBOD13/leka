#include "lob/order/order.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>

int main() {
	const lob::Timestamp timestamp{1};

	// --- construction and getters ---
	lob::Order order{
		lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
	assert(order.getOrderId() == lob::OrderId{1});
	assert(order.getPrice().getPrice() == 100);
	assert(order.getOriginalQuantity().getQuantity() == 10);
	assert(order.getRemainingQuantity().getQuantity() == 10); // single-quantity ctor starts fully unfilled
	assert(order.getTimestamp() == timestamp);
	assert(order.getOrderSide() == lob::OrderSide::BUY);
	assert(order.getOrderType() == lob::OrderType::LIMIT);
	assert(order.getSequenceNumber().getSequenceNumber() == 1);
	assert(order.isBuy() && !order.isSell());
	assert(order.isLimit() && !order.isMarket());
	assert(order.isValid());
	assert(!order.isFullyFilled());

	// The explicit-quantities constructor is what OrderBook uses for a
	// partially-filled resting remainder: original and remaining differ.
	lob::Order partial{
		lob::OrderId{2}, lob::Price{100}, lob::Quantity{100}, lob::Quantity{40},
		timestamp, lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{2}};
	assert(partial.getOriginalQuantity().getQuantity() == 100);
	assert(partial.getRemainingQuantity().getQuantity() == 40);
	assert(partial.isValid());

	// --- intrusive list / price level links: plain getters/setters ---
	lob::Order neighbor{
		lob::OrderId{3}, lob::Price{100}, lob::Quantity{5}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{3}};
	assert(order.getNextOrder() == nullptr);
	assert(order.getPreviousOrder() == nullptr);
	assert(order.getPriceLevel() == nullptr);
	order.setNextOrder(&neighbor);
	neighbor.setPreviousOrder(&order);
	assert(order.getNextOrder() == &neighbor);
	assert(neighbor.getPreviousOrder() == &order);
	order.setNextOrder(nullptr);
	neighbor.setPreviousOrder(nullptr);

	// --- reduceRemainingQuantity ---
	{
		lob::Order o{
			lob::OrderId{4}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{4}};
		o.reduceRemainingQuantity(lob::Quantity{4});
		assert(o.getRemainingQuantity().getQuantity() == 6);
		assert(!o.isFullyFilled());

		o.reduceRemainingQuantity(lob::Quantity{6});
		assert(o.getRemainingQuantity().getQuantity() == 0);
		assert(o.isFullyFilled());

		try {
			o.reduceRemainingQuantity(lob::Quantity{1});
			assert(false);
		} catch (const std::invalid_argument&) {
		}
	}

	// --- setRemainingQuantity ---
	{
		lob::Order o{
			lob::OrderId{5}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{5}};
		o.setRemainingQuantity(lob::Quantity{3}); // a REDUCE-style shrink
		assert(o.getRemainingQuantity().getQuantity() == 3);

		try {
			o.setRemainingQuantity(lob::Quantity{}); // zero is invalid, not "fully filled"
			assert(false);
		} catch (const std::invalid_argument&) {
		}

		try {
			o.setRemainingQuantity(lob::Quantity{11}); // above originalQuantity
			assert(false);
		} catch (const std::invalid_argument&) {
		}
		assert(o.getRemainingQuantity().getQuantity() == 3); // rejected calls leave state unchanged
	}

	// --- isValid(): each field independently gates validity ---
	{
		// Invalid order ID.
		lob::Order o{
			lob::OrderId{}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		assert(!o.isValid());
	}
	{
		// Invalid quantity.
		lob::Order o{
			lob::OrderId{1}, lob::Price{100}, lob::Quantity{}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		assert(!o.isValid());
	}
	{
		// remaining > original is only reachable through the explicit
		// two-quantity constructor; OrderBook never does this, but isValid()
		// must still catch it if it happens.
		lob::Order o{
			lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, lob::Quantity{20},
			timestamp, lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		assert(!o.isValid());
	}
	{
		// Invalid timestamp.
		lob::Order o{
			lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, lob::Timestamp{},
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		assert(!o.isValid());
	}
	{
		// Invalid sequence number.
		lob::Order o{
			lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{}};
		assert(!o.isValid());
	}
	{
		// A side or type outside the declared enumerators can only arise from
		// a corrupted or miscast value, which is exactly what isValid()'s
		// validSide/validType checks exist to catch.
		lob::Order badSide{
			lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, timestamp,
			static_cast<lob::OrderSide>(99), lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		assert(!badSide.isValid());

		lob::Order badType{
			lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, static_cast<lob::OrderType>(99), lob::SequenceNumber{1}};
		assert(!badType.isValid());
	}
	{
		// A LIMIT order needs a valid price...
		lob::Order limitNoPrice{
			lob::OrderId{1}, lob::Price{}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		assert(!limitNoPrice.isValid());

		// ...but a MARKET order does not: an absent price is exactly what a
		// market order is supposed to have.
		lob::Order marketNoPrice{
			lob::OrderId{1}, lob::Price{}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::MARKET, lob::SequenceNumber{1}};
		assert(marketNoPrice.isValid());

		// isValid() does not go the other way: it never rejects a MARKET
		// order for CARRYING a price. "market order must not specify a
		// price" is a stricter rule than Order::isValid() enforces — see
		// MatchingEngine::matchOrder, which is where that rule actually
		// lives. This is worth pinning explicitly so a future reader does
		// not assume isValid() alone is the full acceptance contract.
		lob::Order marketWithPrice{
			lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::MARKET, lob::SequenceNumber{1}};
		assert(marketWithPrice.isValid());
	}

	return 0;
}
