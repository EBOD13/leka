// include/lob/book/order_book.hpp

#ifndef ORDER_BOOK_HPP
#define ORDER_BOOK_HPP

#include "lob/book/order_pool.hpp"
#include "lob/index/order_index.hpp"
#include "lob/book/price_level.hpp"

#include <cstddef>
#include <map>

namespace lob {

class OrderBook {
	public:
		OrderBook() = default;
		OrderBook(const OrderBook&) = delete;
		OrderBook& operator=(const OrderBook&) = delete;
		OrderBook(OrderBook&&) = delete;
		OrderBook& operator=(OrderBook&&) = delete;
		~OrderBook() = default;

		Order* addOrder(OrderId orderId, Price price, Quantity quantity,
						Timestamp timestamp, OrderSide orderSide,
						OrderType orderType, SequenceNumber sequenceNumber);

		bool cancelOrder(const OrderId& orderId); // Cancel an order by its OrderId

        // Find an order by its OrderId
		Order* findOrder(const OrderId& orderId);
		const Order* findOrder(const OrderId& orderId) const;
        
        // Get the best bid and ask price levels
		const PriceLevel* getBestBid() const;
		const PriceLevel* getBestAsk() const;
        
        // Get the count of bid and ask price levels
		std::size_t getBidLevelCount() const { return bids.size(); }
		std::size_t getAskLevelCount() const { return asks.size(); }

	private:
		using PriceLevels = std::map<Price, PriceLevel>;

		OrderPool orderPool;
		OrderIndex orderIndex;
		PriceLevels bids;
		PriceLevels asks;
};

} // namespace lob

#endif // ORDER_BOOK_HPP
