#include "lob/book/order_book.hpp"

#include <cassert>
#include <stdexcept>

int main() {
	lob::OrderBook book; // Create an instance of the OrderBook class
	const lob::Timestamp timestamp{1}; // Use a fixed timestamp for testing

	lob::Order* firstBuy = book.addOrder(
		lob::OrderId{1}, lob::Price{100}, lob::Quantity{10}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{1}); // Add a buy order with OrderId 1, Price 100, Quantity 10, and SequenceNumber 1

	lob::Order* secondBuy = book.addOrder(
		lob::OrderId{2}, lob::Price{100}, lob::Quantity{20}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{2}); // Add a buy order with OrderId 2, Price 100, Quantity 20, and SequenceNumber 2
	
    lob::Order* thirdBuy = book.addOrder(
		lob::OrderId{3}, lob::Price{100}, lob::Quantity{30}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{3}); // Add a buy order with OrderId 3, Price 100, Quantity 30, and SequenceNumber 3
	
    lob::Order* bestBuy = book.addOrder(
		lob::OrderId{4}, lob::Price{101}, lob::Quantity{5}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{4}); // Add a buy order with OrderId 4, Price 101, Quantity 5, and SequenceNumber 4
	
    lob::Order* bestAsk = book.addOrder(
		lob::OrderId{5}, lob::Price{102}, lob::Quantity{7}, timestamp,
		lob::OrderSide::SELL, lob::OrderType::LIMIT, lob::SequenceNumber{5}); // Add a sell order with OrderId 5, Price 102, Quantity 7, and SequenceNumber 5

	const lob::PriceLevel* bidLevel = book.getBestBid();
	const lob::PriceLevel* askLevel = book.getBestAsk();
	assert(bidLevel != nullptr && bidLevel->getPrice().getPrice() == 101);
	assert(askLevel != nullptr && askLevel->getPrice().getPrice() == 102);
	assert(firstBuy->getPriceLevel() != nullptr);
	assert(firstBuy->getPriceLevel()->getHeadOrder() == firstBuy);
	assert(firstBuy->getPriceLevel()->getTailOrder() == thirdBuy);
	assert(firstBuy->getPriceLevel()->getTotalQuantity().getQuantity() == 60);
	assert(firstBuy->getNextOrder() == secondBuy);
	assert(secondBuy->getPreviousOrder() == firstBuy);
	assert(secondBuy->getNextOrder() == thirdBuy);
	assert(thirdBuy->getPreviousOrder() == secondBuy);
	assert(firstBuy->getPreviousOrder() == nullptr);
	assert(thirdBuy->getNextOrder() == nullptr);
	assert(book.findOrder(lob::OrderId{1}) == firstBuy);
	assert(book.findOrder(lob::OrderId{5}) == bestAsk);
	assert(bestBuy->getPriceLevel() == bidLevel);

	assert(book.cancelOrder(lob::OrderId{2}));
	assert(firstBuy->getNextOrder() == thirdBuy);
	assert(thirdBuy->getPreviousOrder() == firstBuy);
	assert(firstBuy->getPriceLevel()->getHeadOrder() == firstBuy);
	assert(firstBuy->getPriceLevel()->getTailOrder() == thirdBuy);
	assert(firstBuy->getPriceLevel()->getOrderCount() == 2);
	assert(firstBuy->getPriceLevel()->getTotalQuantity().getQuantity() == 40);
	assert(secondBuy->getPreviousOrder() == nullptr);
	assert(secondBuy->getNextOrder() == nullptr);
	assert(secondBuy->getPriceLevel() == nullptr);

	try {
		book.addOrder(
			lob::OrderId{1}, lob::Price{103}, lob::Quantity{1}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{5});
		assert(false);
	} catch (const std::logic_error&) {
	}
	assert(book.findOrder(lob::OrderId{1}) == firstBuy);
	assert(book.getBidLevelCount() == 2);

	try {
		book.addOrder(
			lob::OrderId{8}, lob::Price{}, lob::Quantity{1}, timestamp,
			lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{5});
		assert(false);
	} catch (const std::invalid_argument&) {
	}
	assert(book.findOrder(lob::OrderId{8}) == nullptr);
	assert(book.getBidLevelCount() == 2);

	try {
		book.addOrder(
			lob::OrderId{6}, lob::Price{}, lob::Quantity{1}, timestamp,
			lob::OrderSide::SELL, lob::OrderType::MARKET, lob::SequenceNumber{6});
		assert(false);
	} catch (const std::invalid_argument&) {
	}
	assert(book.findOrder(lob::OrderId{6}) == nullptr);
	assert(book.getAskLevelCount() == 1);

	assert(book.cancelOrder(lob::OrderId{1}));
	assert(book.findOrder(lob::OrderId{1}) == nullptr);
	assert(firstBuy->getPriceLevel() == nullptr);
	assert(book.getBidLevelCount() == 2);

	lob::Order* reused = book.addOrder(
		lob::OrderId{7}, lob::Price{100}, lob::Quantity{4}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT, lob::SequenceNumber{7});
	assert(reused == firstBuy);

	assert(book.cancelOrder(lob::OrderId{7}));
	assert(book.cancelOrder(lob::OrderId{3}));
	assert(book.cancelOrder(lob::OrderId{4}));
	assert(book.cancelOrder(lob::OrderId{5}));
	assert(!book.cancelOrder(lob::OrderId{999}));
	assert(book.getBidLevelCount() == 0);
	assert(book.getAskLevelCount() == 0);
	assert(book.getBestBid() == nullptr);
	assert(book.getBestAsk() == nullptr);

	lob::OrderBook remainderBook;
	lob::Order* remainder = remainderBook.addRestingRemainder(
		lob::OrderId{10}, lob::Price{105}, lob::Quantity{100},
		lob::Quantity{60}, timestamp, lob::OrderSide::BUY,
		lob::OrderType::LIMIT, lob::SequenceNumber{10});
	assert(remainder->getOriginalQuantity().getQuantity() == 100);
	assert(remainder->getRemainingQuantity().getQuantity() == 60);
	assert(remainderBook.getBestBid()->getTotalQuantity().getQuantity() == 60);
}
