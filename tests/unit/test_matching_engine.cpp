#include "lob/matching/matching_engine.hpp"

#include <cassert>
#include <stdexcept>

int main() {
	lob::OrderBook book;
	lob::MatchingEngine engine{book};
	const lob::Timestamp timestamp{1};

	book.addOrder(
		lob::OrderId{1}, lob::Price{100}, lob::Quantity{40}, timestamp,
		lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{1});
	book.addOrder(
		lob::OrderId{2}, lob::Price{100}, lob::Quantity{30}, timestamp,
		lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{2});
	book.addOrder(
		lob::OrderId{3}, lob::Price{102}, lob::Quantity{50}, timestamp,
		lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{3});

	const auto executions = engine.processOrder(
		lob::OrderId{10}, lob::Price{105}, lob::Quantity{150}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT);

	assert(executions.size() == 3);
	assert(executions[0].getRestingOrderId() == lob::OrderId{1});
	assert(executions[0].getExecutionPrice().getPrice() == 100);
	assert(executions[0].getExecutionQuantity().getQuantity() == 40);
	assert(executions[1].getRestingOrderId() == lob::OrderId{2});
	assert(executions[1].getExecutionQuantity().getQuantity() == 30);
	assert(executions[2].getRestingOrderId() == lob::OrderId{3});
	assert(executions[2].getExecutionPrice().getPrice() == 102);
	assert(executions[2].getExecutionQuantity().getQuantity() == 50);
	assert(book.findOrder(lob::OrderId{2}) == nullptr);
	assert(book.getBestAsk() == nullptr);

	const lob::Order* remainder = book.findOrder(lob::OrderId{10});
	assert(remainder != nullptr);
	assert(remainder->getOriginalQuantity().getQuantity() == 150);
	assert(remainder->getRemainingQuantity().getQuantity() == 30);
	assert(remainder->getPrice().getPrice() == 105);

	book.addOrder(
		lob::OrderId{4}, lob::Price{104}, lob::Quantity{20}, timestamp,
		lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{4});

	const auto marketExecutions = engine.processOrder(
		lob::OrderId{11}, lob::Price{}, lob::Quantity{100}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::MARKET);
	assert(marketExecutions.size() == 1);
	assert(marketExecutions[0].getRestingOrderId() == lob::OrderId{4});
	assert(marketExecutions[0].getExecutionQuantity().getQuantity() == 20);
	assert(book.findOrder(lob::OrderId{11}) == nullptr);
	assert(book.findOrder(lob::OrderId{3}) == nullptr);

	try {
		engine.processOrder(
			lob::OrderId{12}, lob::Price{100}, lob::Quantity{1}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::MARKET);
		assert(false);
	} catch (const std::invalid_argument&) {
	}

	lob::OrderBook sellBook;
	lob::MatchingEngine sellEngine{sellBook};
	lob::Order* bid = sellBook.addOrder(
		lob::OrderId{20}, lob::Price{99}, lob::Quantity{25}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{20});
	const auto sellExecutions = sellEngine.processOrder(
		lob::OrderId{21}, lob::Price{98}, lob::Quantity{10}, timestamp,
		lob::OrderSide::SELL, lob::OrderType::LIMIT);
	assert(sellExecutions.size() == 1);
	assert(sellExecutions[0].getRestingOrderId() == lob::OrderId{20});
	assert(sellExecutions[0].getExecutionPrice().getPrice() == 99);
	assert(sellExecutions[0].getExecutionQuantity().getQuantity() == 10);
	assert(bid->getRemainingQuantity().getQuantity() == 15);
	assert(sellBook.getBestBid()->getTotalQuantity().getQuantity() == 15);
	assert(sellBook.findOrder(lob::OrderId{21}) == nullptr);

	{
		lob::OrderBook nonCrossingBook;
		lob::MatchingEngine nonCrossingEngine{nonCrossingBook};
		nonCrossingBook.addOrder(
			lob::OrderId{30}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{30});
		const auto nonCrossing = nonCrossingEngine.processOrder(
			lob::OrderId{31}, lob::Price{99}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT);
		assert(nonCrossing.empty());
		assert(nonCrossingBook.findOrder(lob::OrderId{31}) != nullptr);

		const auto exactCrossing = nonCrossingEngine.processOrder(
			lob::OrderId{32}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT);
		assert(exactCrossing.size() == 1);
		assert(exactCrossing[0].getExecutionPrice().getPrice() == 100);
		assert(nonCrossingBook.findOrder(lob::OrderId{30}) == nullptr);
	}

	{
		lob::OrderBook fifoBook;
		lob::MatchingEngine fifoEngine{fifoBook};
		lob::Order* first = fifoBook.addOrder(
			lob::OrderId{40}, lob::Price{100}, lob::Quantity{100}, timestamp,
			lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{40});
		lob::Order* second = fifoBook.addOrder(
			lob::OrderId{41}, lob::Price{100}, lob::Quantity{100}, timestamp,
			lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{41});
		const auto partialFifo = fifoEngine.processOrder(
			lob::OrderId{42}, lob::Price{105}, lob::Quantity{50}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT);
		assert(partialFifo.size() == 1);
		assert(partialFifo[0].getRestingOrderId() == lob::OrderId{40});
		assert(first->getRemainingQuantity().getQuantity() == 50);
		assert(second->getRemainingQuantity().getQuantity() == 100);
		assert(fifoBook.findOrder(lob::OrderId{42}) == nullptr);
	}

	{
		lob::OrderBook marketSellBook;
		lob::MatchingEngine marketSellEngine{marketSellBook};
		marketSellBook.addOrder(
			lob::OrderId{50}, lob::Price{100}, lob::Quantity{25}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{50});
		const auto marketSell = marketSellEngine.processOrder(
			lob::OrderId{51}, lob::Price{}, lob::Quantity{30}, timestamp,
			lob::OrderSide::SELL, lob::OrderType::MARKET);
		assert(marketSell.size() == 1);
		assert(marketSell[0].getExecutionQuantity().getQuantity() == 25);
		assert(marketSellBook.findOrder(lob::OrderId{51}) == nullptr);
	}

	{
		lob::OrderBook remainderBook;
		lob::MatchingEngine remainderEngine{remainderBook};
		remainderBook.addOrder(
			lob::OrderId{60}, lob::Price{100}, lob::Quantity{5}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{60});
		const auto sellRemainder = remainderEngine.processOrder(
			lob::OrderId{61}, lob::Price{95}, lob::Quantity{10}, timestamp,
			lob::OrderSide::SELL, lob::OrderType::LIMIT);
		assert(sellRemainder.size() == 1);
		const lob::Order* restingSell = remainderBook.findOrder(lob::OrderId{61});
		assert(restingSell != nullptr);
		assert(restingSell->getOriginalQuantity().getQuantity() == 10);
		assert(restingSell->getRemainingQuantity().getQuantity() == 5);
	}

	{
		lob::OrderBook emptyBook;
		lob::MatchingEngine emptyEngine{emptyBook};
		assert(emptyEngine.processOrder(
			lob::OrderId{70}, lob::Price{}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::MARKET).empty());
		assert(emptyBook.findOrder(lob::OrderId{70}) == nullptr);
		assert(emptyEngine.processOrder(
			lob::OrderId{71}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT).empty());
		assert(emptyBook.findOrder(lob::OrderId{71}) != nullptr);
	}

	{
		lob::OrderBook duplicateBook;
		lob::MatchingEngine duplicateEngine{duplicateBook};
		duplicateBook.addOrder(
			lob::OrderId{80}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{80});
		try {
			duplicateEngine.processOrder(
				lob::OrderId{80}, lob::Price{105}, lob::Quantity{1}, timestamp,
				lob::OrderSide::BUY, lob::OrderType::LIMIT);
			assert(false);
		} catch (const std::logic_error&) {
		}
	}

	{
		lob::OrderBook eventBook;
		lob::MatchingEngine eventEngine{eventBook};
		const auto newEvent = eventEngine.processEvent(lob::OrderEvent{
			lob::NewOrder{lob::OrderId{100}, lob::Price{100}, lob::Quantity{10},
							  timestamp, lob::OrderSide::BUY, lob::OrderType::LIMIT}});
		assert(newEvent.empty());
		lob::Order* first = eventBook.findOrder(lob::OrderId{100});
		assert(first != nullptr);
		const auto originalSequence = first->getSequenceNumber();

		eventEngine.processEvent(lob::OrderEvent{
			lob::NewOrder{lob::OrderId{101}, lob::Price{100}, lob::Quantity{20},
							  timestamp, lob::OrderSide::BUY, lob::OrderType::LIMIT}});
		// REDUCE shrinks in place: the order keeps its sequence number and
		// stays at the head of its level.
		eventEngine.processEvent(lob::OrderEvent{
			lob::ReduceOrder{lob::OrderId{100}, lob::Quantity{5}}});
		first = eventBook.findOrder(lob::OrderId{100});
		assert(first->getRemainingQuantity().getQuantity() == 5);
		assert(first->getSequenceNumber().getSequenceNumber() ==
			originalSequence.getSequenceNumber());
		assert(eventBook.getBestBid()->getTotalQuantity().getQuantity() == 25);
		assert(eventBook.getBestBid()->getHeadOrder()->getOrderId() ==
			lob::OrderId{100});

		// A size increase is not a reduction and must be rejected rather than
		// silently keeping priority.
		try {
			eventEngine.processEvent(lob::OrderEvent{
				lob::ReduceOrder{lob::OrderId{100}, lob::Quantity{15}}});
			assert(false);
		} catch (const std::invalid_argument&) {
		}

		// Reducing an order that is not in the book is a no-op, not an error:
		// a replayed feed can reference an order from before the window.
		const auto missing = eventEngine.processEvent(lob::OrderEvent{
			lob::ReduceOrder{lob::OrderId{999}, lob::Quantity{1}}});
		assert(missing.empty());

		// A reprice is CANCEL followed by NEW, which forfeits priority: order
		// 101 was behind 100 at this price and now leads it.
		eventEngine.processEvent(lob::OrderEvent{
			lob::CancelOrder{lob::OrderId{100}}});
		eventEngine.processEvent(lob::OrderEvent{
			lob::NewOrder{lob::OrderId{102}, lob::Price{100}, lob::Quantity{5},
							  timestamp, lob::OrderSide::BUY, lob::OrderType::LIMIT}});
		assert(eventBook.findOrder(lob::OrderId{100}) == nullptr);
		assert(eventBook.getBestBid()->getHeadOrder()->getOrderId() ==
			lob::OrderId{101});

		eventEngine.processEvent(lob::OrderEvent{
			lob::CancelOrder{lob::OrderId{101}}});
		assert(eventBook.findOrder(lob::OrderId{101}) == nullptr);
	}

	{
		// Repricing an order through the opposite side must trade rather than
		// leave the book crossed. Expressed as CANCEL + NEW, it runs the
		// matcher, which the removed in-place MODIFY path never did.
		lob::OrderBook crossBook;
		lob::MatchingEngine crossEngine{crossBook};
		crossEngine.processEvent(lob::OrderEvent{
			lob::NewOrder{lob::OrderId{200}, lob::Price{100}, lob::Quantity{10},
							  timestamp, lob::OrderSide::BUY, lob::OrderType::LIMIT}});
		crossEngine.processEvent(lob::OrderEvent{
			lob::NewOrder{lob::OrderId{201}, lob::Price{105}, lob::Quantity{10},
							  timestamp, lob::OrderSide::SELL, lob::OrderType::LIMIT}});

		crossEngine.processEvent(lob::OrderEvent{lob::CancelOrder{lob::OrderId{200}}});
		const auto crossed = crossEngine.processEvent(lob::OrderEvent{
			lob::NewOrder{lob::OrderId{202}, lob::Price{106}, lob::Quantity{10},
							  timestamp, lob::OrderSide::BUY, lob::OrderType::LIMIT}});

		assert(crossed.size() == 1);
		assert(crossed[0].getRestingOrderId() == lob::OrderId{201});
		assert(crossed[0].getExecutionPrice().getPrice() == 105);
		assert(crossed[0].getExecutionQuantity().getQuantity() == 10);
		assert(crossBook.getAskLevelCount() == 0);
		assert(crossBook.getBidLevelCount() == 0);
	}

	{
		lob::OrderBook sequenceBook;
		lob::MatchingEngine sequenceEngine{sequenceBook};
		sequenceEngine.processOrder(
			lob::OrderId{90}, lob::Price{100}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT);
		const lob::Order* firstAccepted =
			sequenceBook.findOrder(lob::OrderId{90});
		assert(firstAccepted != nullptr);

		try {
			sequenceEngine.processOrder(
				lob::OrderId{91}, lob::Price{100}, lob::Quantity{1}, timestamp,
				lob::OrderSide::BUY, lob::OrderType::MARKET);
			assert(false);
		} catch (const std::invalid_argument&) {
		}

		sequenceEngine.processOrder(
			lob::OrderId{92}, lob::Price{101}, lob::Quantity{10}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT);
		const lob::Order* secondAccepted =
			sequenceBook.findOrder(lob::OrderId{92});
		assert(secondAccepted != nullptr);
		assert(secondAccepted->getSequenceNumber().getSequenceNumber() ==
			firstAccepted->getSequenceNumber().getSequenceNumber() + 1);
	}

	return 0;
}
