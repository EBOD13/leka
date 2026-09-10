// include/lob/book/order_book.hpp

#ifndef ORDER_BOOK_HPP
#define ORDER_BOOK_HPP

#include "lob/book/order_pool.hpp"
#include "lob/index/order_index.hpp"
#include "lob/book/price_level.hpp"

#include <cstddef>
#include <map>

namespace lob {

/**
 * @brief Owns resting limit orders and maintains their book indexes.
 *
 * OrderBook owns Order storage through OrderPool. OrderIndex and PriceLevel
 * objects hold non-owning references to those orders. Matching decisions are
 * made by MatchingEngine; this class maintains synchronized book state.
 */
class OrderBook {
	public:
		/** Creates an empty order book. */
		OrderBook() = default;
		OrderBook(const OrderBook&) = delete;
		OrderBook& operator=(const OrderBook&) = delete;
		OrderBook(OrderBook&&) = delete;
		OrderBook& operator=(OrderBook&&) = delete;
		~OrderBook() = default;

		/**
		 * @brief Adds a valid resting limit order to the appropriate side.
		 *
		 * The book allocates the order, links it into the side's PriceLevel FIFO,
		 * and registers it in OrderIndex. If any step fails, all earlier state is
		 * rolled back before the pool slot is released.
		 * @return The stable address of the newly stored order.
		 * @throws std::invalid_argument if the order is invalid or not a limit.
		 * @throws std::logic_error if the order ID already exists.
		 */
		Order* addOrder(OrderId orderId, Price price, Quantity quantity,
						Timestamp timestamp, OrderSide orderSide,
						OrderType orderType, SequenceNumber sequenceNumber);

		/**
		 * @brief Adds a partially filled limit order while preserving its history.
		 *
		 * This is used when MatchingEngine has executed part of an incoming order.
		 * The stored order retains the submitted quantity as originalQuantity and
		 * enters the book with only remainingQuantity available.
		 * @param originalQuantity Quantity submitted before any executions.
		 * @param remainingQuantity Quantity still available to execute.
		 * @throws std::invalid_argument if remainingQuantity is zero, exceeds the
		 * original quantity, or the order is otherwise invalid.
		 */
		Order* addRestingRemainder(
			OrderId orderId, Price price, Quantity originalQuantity,
			Quantity remainingQuantity, Timestamp timestamp, OrderSide orderSide,
			OrderType orderType, SequenceNumber sequenceNumber);

		/**
		 * @brief Removes a resting order by ID.
		 * @return false when no order with the ID is present; true after removal.
		 *
		 * Removal is delegated to removeOrder(Order*) so the PriceLevel, OrderIndex,
		 * and OrderPool remain synchronized.
		 */
		bool cancelOrder(const OrderId& orderId);

		/**
		 * @brief Applies Nasdaq-style modification semantics to a resting order.
		 *
		 * A same-price quantity decrease is applied in place and preserves
		 * sequence priority. A quantity increase or price change removes and
		 * re-adds the order at the FIFO tail using newSequenceNumber.
		 * @return false when the order is absent; true after modification.
		 * @throws std::invalid_argument for invalid values or a missing reset
		 * sequence number.
		 */
		bool modifyOrder(const OrderId& orderId, Price newPrice,
						 Quantity newQuantity, SequenceNumber newSequenceNumber);

		/**
		 * @brief Removes a currently resting order from every book structure.
		 *
		 * The order is unlinked from its FIFO and its empty PriceLevel is erased
		 * before its index entry and pool storage are released. The caller must not
		 * use the pointer after this function returns.
		 */
		void removeOrder(Order* order);

		/** Finds an order by ID, or returns nullptr when absent. */
		Order* findOrder(const OrderId& orderId);
		/** Finds an order by ID without allowing mutation. */
		const Order* findOrder(const OrderId& orderId) const;

		/**
		 * @brief Returns the mutable highest-priced bid level.
		 * @return The best bid level, or nullptr when no bids are resting.
		 */
		PriceLevel* getBestBid();
		/**
		 * @brief Returns the mutable lowest-priced ask level.
		 * @return The best ask level, or nullptr when no asks are resting.
		 */
		PriceLevel* getBestAsk();
		/** @brief Returns the best bid level for read-only inspection. */
		const PriceLevel* getBestBid() const;
		/** @brief Returns the best ask level for read-only inspection. */
		const PriceLevel* getBestAsk() const;

		/** Returns the number of bid price levels. */
		std::size_t getBidLevelCount() const { return bids.size(); }
		/** Returns the number of ask price levels. */
		std::size_t getAskLevelCount() const { return asks.size(); }

	private:
		using PriceLevels = std::map<Price, PriceLevel>;

		OrderPool orderPool;
		OrderIndex orderIndex;
		PriceLevels bids;
		PriceLevels asks;

		Order* addOrderWithQuantities(
			OrderId orderId, Price price, Quantity originalQuantity,
			Quantity remainingQuantity, Timestamp timestamp, OrderSide orderSide,
			OrderType orderType, SequenceNumber sequenceNumber);
};

} // namespace lob

#endif // ORDER_BOOK_HPP
