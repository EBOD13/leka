#include "lob/book/price_level.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>

int main() {
	const lob::Timestamp timestamp{1};

	// --- FIFO ordering and aggregate tracking across several adds ---
	{
		lob::PriceLevel level{lob::Price{100}};
		assert(level.isEmpty());
		assert(level.getOrderCount() == 0);
		assert(level.getTotalQuantity().getQuantity() == 0);
		assert(level.getHeadOrder() == nullptr);
		assert(level.getTailOrder() == nullptr);

		lob::Order first{
			lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		lob::Order second{
			lob::OrderId{2}, lob::Price{100}, lob::Quantity{20}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{2}};
		lob::Order third{
			lob::OrderId{3}, lob::Price{100}, lob::Quantity{30}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{3}};

		level.addOrder(&first);
		assert(!level.isEmpty());
		assert(level.getHeadOrder() == &first);
		assert(level.getTailOrder() == &first);
		assert(level.getOrderCount() == 1);
		assert(level.getTotalQuantity().getQuantity() == 10);
		assert(first.getPriceLevel() == &level);

		level.addOrder(&second);
		assert(level.getHeadOrder() == &first); // FIFO: head never moves on append
		assert(level.getTailOrder() == &second);
		assert(first.getNextOrder() == &second);
		assert(second.getPreviousOrder() == &first);
		assert(level.getOrderCount() == 2);
		assert(level.getTotalQuantity().getQuantity() == 30);

		level.addOrder(&third);
		assert(level.getTailOrder() == &third);
		assert(second.getNextOrder() == &third);
		assert(third.getPreviousOrder() == &second);
		assert(level.getOrderCount() == 3);
		assert(level.getTotalQuantity().getQuantity() == 60);

		// --- remove from the middle: FIFO must relink around it ---
		level.removeOrder(&second);
		assert(level.getHeadOrder() == &first);
		assert(level.getTailOrder() == &third);
		assert(first.getNextOrder() == &third);
		assert(third.getPreviousOrder() == &first);
		assert(level.getOrderCount() == 2);
		assert(level.getTotalQuantity().getQuantity() == 40);
		// A removed order is fully unlinked and detached from the level.
		assert(second.getNextOrder() == nullptr);
		assert(second.getPreviousOrder() == nullptr);
		assert(second.getPriceLevel() == nullptr);

		// --- remove the head ---
		level.removeOrder(&first);
		assert(level.getHeadOrder() == &third);
		assert(level.getTailOrder() == &third);
		assert(third.getPreviousOrder() == nullptr);
		assert(level.getOrderCount() == 1);
		assert(level.getTotalQuantity().getQuantity() == 30);

		// --- remove the last remaining order: level goes empty ---
		level.removeOrder(&third);
		assert(level.isEmpty());
		assert(level.getHeadOrder() == nullptr);
		assert(level.getTailOrder() == nullptr);
		assert(level.getOrderCount() == 0);
		assert(level.getTotalQuantity().getQuantity() == 0);
	}

	// --- addOrder rejects what it must ---
	{
		lob::PriceLevel level{lob::Price{100}};
		lob::PriceLevel otherLevel{lob::Price{101}};

		try {
			level.addOrder(nullptr);
			assert(false);
		} catch (const std::invalid_argument&) {
		}

		lob::Order wrongPrice{
			lob::OrderId{1}, lob::Price{101}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		try {
			level.addOrder(&wrongPrice); // 101 does not belong on the 100 level
			assert(false);
		} catch (const std::invalid_argument&) {
		}

		lob::Order alreadyLinked{
			lob::OrderId{2}, lob::Price{101}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{2}};
		otherLevel.addOrder(&alreadyLinked);
		try {
			otherLevel.addOrder(&alreadyLinked); // already linked to otherLevel
			assert(false);
		} catch (const std::invalid_argument&) {
		}

		lob::Order filled{
			lob::OrderId{3}, lob::Price{100}, lob::Quantity{10}, lob::Quantity{0},
			timestamp, lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{3}};
		try {
			level.addOrder(&filled); // zero remaining quantity
			assert(false);
		} catch (const std::invalid_argument&) {
		}

		assert(level.isEmpty()); // none of the rejected attempts left a trace
	}

	// --- addOrder rejects an aggregate quantity overflow ---
	{
		lob::PriceLevel level{lob::Price{100}};
		lob::Order huge{
			lob::OrderId{1}, lob::Price{100},
			lob::Quantity{std::numeric_limits<std::uint64_t>::max() - 5}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		level.addOrder(&huge);

		lob::Order pushesOverTop{
			lob::OrderId{2}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{2}};
		try {
			level.addOrder(&pushesOverTop);
			assert(false);
		} catch (const std::overflow_error&) {
		}
		// The failed add must not have partially mutated the level.
		assert(level.getOrderCount() == 1);
		assert(level.getTotalQuantity().getQuantity() ==
			   std::numeric_limits<std::uint64_t>::max() - 5);
	}

	// --- removeOrder rejects what it must ---
	{
		lob::PriceLevel level{lob::Price{100}};
		lob::PriceLevel otherLevel{lob::Price{101}};

		try {
			level.removeOrder(nullptr);
			assert(false);
		} catch (const std::invalid_argument&) {
		}

		lob::Order elsewhere{
			lob::OrderId{1}, lob::Price{101}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		otherLevel.addOrder(&elsewhere);
		try {
			level.removeOrder(&elsewhere); // belongs to otherLevel, not level
			assert(false);
		} catch (const std::invalid_argument&) {
		}

		// Never having been added at all is the same failure mode: its
		// getPriceLevel() is nullptr, never this level's address.
		lob::Order neverAdded{
			lob::OrderId{2}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{2}};
		try {
			level.removeOrder(&neverAdded);
			assert(false);
		} catch (const std::invalid_argument&) {
		}
	}

	// --- reduceTotalQuantity: the partial-fill path ---
	{
		lob::PriceLevel level{lob::Price{100}};
		lob::Order order{
			lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}};
		level.addOrder(&order);

		level.reduceTotalQuantity(lob::Quantity{4});
		assert(level.getTotalQuantity().getQuantity() == 6);
		// reduceTotalQuantity intentionally does not touch order count or the
		// FIFO links: it exists for the matching engine's partial-execution
		// path, where the order itself stays resting and is not removed.
		assert(level.getOrderCount() == 1);
		assert(order.getPriceLevel() == &level);

		try {
			level.reduceTotalQuantity(lob::Quantity{7}); // more than what remains
			assert(false);
		} catch (const std::invalid_argument&) {
		}
		assert(level.getTotalQuantity().getQuantity() == 6); // rejected call left state unchanged
	}

	return 0;
}
