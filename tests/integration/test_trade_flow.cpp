#include "lob/matching/matching_engine.hpp"

#include <cassert>
#include <vector>

/**
 * @file
 * @brief One continuous trading session through MatchingEngine + OrderBook.
 *
 * Unlike test_matching_engine.cpp's isolated, per-feature blocks, this test
 * runs a single book through a realistic sequence: a multi-level sweep that
 * exhausts a price level mid-book, a MARKET order that drains the opposite
 * side and discards its unfilled remainder, a REDUCE, a CANCEL, and a fresh
 * NEW arriving into a thin, one-sided book that both fills and rests in the
 * same event. Every event goes through OrderEvent and the buffer-taking
 * processEvent() overload, matching how a real caller uses this engine.
 */

int main() {
	lob::OrderBook book;
	lob::MatchingEngine engine{book};
	const lob::Timestamp timestamp{1};
	std::vector<lob::Execution> out;

	const auto newOrder = [&](std::uint64_t id, std::uint64_t price, std::uint64_t qty,
							  lob::OrderSide side) {
		engine.processEvent(lob::OrderEvent{lob::NewOrder{
			lob::OrderId{id}, lob::Price{price}, lob::Quantity{qty}, timestamp,
			side, lob::OrderType::LIMIT}}, out);
	};

	// --- seed a four-level book: two orders at the best ask, single orders elsewhere ---
	newOrder(101, 101, 40, lob::OrderSide::SELL);
	newOrder(102, 101, 30, lob::OrderSide::SELL);
	newOrder(103, 102, 50, lob::OrderSide::SELL);
	newOrder(104, 103, 20, lob::OrderSide::SELL);
	newOrder(201, 99, 25, lob::OrderSide::BUY);
	newOrder(202, 99, 35, lob::OrderSide::BUY);
	newOrder(203, 98, 10, lob::OrderSide::BUY);

	assert(book.getBestBid()->getPrice().getPrice() == 99);
	assert(book.getBestBid()->getTotalQuantity().getQuantity() == 60);
	assert(book.getBestAsk()->getPrice().getPrice() == 101);
	assert(book.getBestAsk()->getTotalQuantity().getQuantity() == 70);
	assert(book.getAskLevelCount() == 3);

	// --- a buy that sweeps the entire 101 level, then partially fills 102 ---
	// 40 (order 101) + 30 (order 102) fully exhausts price 101; the
	// remaining 20 of 90 takes 20 of order 103's 50 at price 102, leaving it
	// resting with 30. The incoming order is fully filled by this and never
	// rests itself.
	engine.processEvent(lob::OrderEvent{lob::NewOrder{
		lob::OrderId{301}, lob::Price{102}, lob::Quantity{90}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT}}, out);

	assert(out.size() == 3);
	assert(out[0].getRestingOrderId() == lob::OrderId{101});
	assert(out[0].getExecutionPrice().getPrice() == 101);
	assert(out[0].getExecutionQuantity().getQuantity() == 40);
	assert(out[1].getRestingOrderId() == lob::OrderId{102});
	assert(out[1].getExecutionPrice().getPrice() == 101);
	assert(out[1].getExecutionQuantity().getQuantity() == 30);
	assert(out[2].getRestingOrderId() == lob::OrderId{103});
	assert(out[2].getExecutionPrice().getPrice() == 102);
	assert(out[2].getExecutionQuantity().getQuantity() == 20);

	assert(book.findOrder(lob::OrderId{301}) == nullptr); // fully filled, never rested
	assert(book.findOrder(lob::OrderId{101}) == nullptr); // level 101 is gone entirely
	assert(book.findOrder(lob::OrderId{102}) == nullptr);
	assert(book.getAskLevelCount() == 2); // 101 removed; 102 and 103 remain
	assert(book.getBestAsk()->getPrice().getPrice() == 102);
	assert(book.getBestAsk()->getTotalQuantity().getQuantity() == 30);
	assert(book.findOrder(lob::OrderId{103})->getRemainingQuantity().getQuantity() == 30);

	// --- a market sell that drains the bid side and discards its remainder ---
	// Available bid depth is 60 (price 99) + 10 (price 98) = 70. The order
	// wants 100; the unfilled 30 is a MARKET remainder and must not rest.
	engine.processEvent(lob::OrderEvent{lob::NewOrder{
		lob::OrderId{302}, lob::Price{}, lob::Quantity{100}, timestamp,
		lob::OrderSide::SELL, lob::OrderType::MARKET}}, out);

	assert(out.size() == 3);
	std::uint64_t totalFilled = 0;
	for (const auto& execution : out) {
		totalFilled += execution.getExecutionQuantity().getQuantity();
	}
	assert(totalFilled == 70);
	assert(book.findOrder(lob::OrderId{302}) == nullptr); // MARKET never rests, filled or not
	assert(book.getBidLevelCount() == 0);
	assert(book.getBestBid() == nullptr);

	// --- REDUCE the resting order at 102: priority must be preserved ---
	const lob::Order* beforeReduce = book.findOrder(lob::OrderId{103});
	const lob::SequenceNumber sequenceBeforeReduce = beforeReduce->getSequenceNumber();
	engine.processEvent(lob::OrderEvent{lob::ReduceOrder{
		lob::OrderId{103}, lob::Quantity{10}}}, out);
	assert(out.empty()); // REDUCE never executes
	const lob::Order* afterReduce = book.findOrder(lob::OrderId{103});
	assert(afterReduce == beforeReduce); // same slot: no requeue happened
	assert(afterReduce->getRemainingQuantity().getQuantity() == 10);
	assert(afterReduce->getSequenceNumber() == sequenceBeforeReduce);
	assert(book.getBestAsk()->getTotalQuantity().getQuantity() == 10);

	// --- CANCEL the other resting order, at 103 ---
	engine.processEvent(lob::OrderEvent{lob::CancelOrder{lob::OrderId{104}}}, out);
	assert(out.empty());
	assert(book.findOrder(lob::OrderId{104}) == nullptr);
	assert(book.getAskLevelCount() == 1);
	assert(book.getBestAsk()->getPrice().getPrice() == 102);

	// --- a fresh buy arrives into the now one-sided, thin book: fills part,
	// rests the rest, in a single event ---
	newOrder(401, 105, 15, lob::OrderSide::BUY);
	assert(out.size() == 1);
	assert(out[0].getRestingOrderId() == lob::OrderId{103});
	assert(out[0].getExecutionPrice().getPrice() == 102);
	assert(out[0].getExecutionQuantity().getQuantity() == 10);
	assert(book.findOrder(lob::OrderId{103}) == nullptr); // fully consumed
	assert(book.getAskLevelCount() == 0);
	assert(book.getBestAsk() == nullptr);
	const lob::Order* remainder = book.findOrder(lob::OrderId{401});
	assert(remainder != nullptr);
	assert(remainder->getOriginalQuantity().getQuantity() == 15);
	assert(remainder->getRemainingQuantity().getQuantity() == 5); // 15 submitted, 10 filled
	assert(book.getBestBid()->getPrice().getPrice() == 105);
	assert(book.getBestBid()->getTotalQuantity().getQuantity() == 5);

	// --- the session ends clean: cancel the last resting order ---
	engine.processEvent(lob::OrderEvent{lob::CancelOrder{lob::OrderId{401}}}, out);
	assert(book.getBidLevelCount() == 0);
	assert(book.getAskLevelCount() == 0);
	assert(book.getBestBid() == nullptr);
	assert(book.getBestAsk() == nullptr);

	return 0;
}
