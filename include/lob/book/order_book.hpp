// include/lob/book/order_book.hpp

#ifndef ORDER_BOOK_HPP
#define ORDER_BOOK_HPP

#include "lob/book/order_pool.hpp"
#include "lob/book/price_ladder.hpp"
#include "lob/index/order_index.hpp"
#include "lob/book/price_level.hpp"

#include <cstddef>

namespace lob {

/**
 * @brief Owns resting limit orders and maintains their book indexes.
 *
 * OrderBook owns Order storage through OrderPool. OrderIndex and PriceLevel
 * objects hold non-owning references to those orders. Matching decisions are
 * made by MatchingEngine; this class maintains synchronized book state.
 *
 * Bid and ask levels are tick-indexed arrays (PriceLadder), not ordered maps:
 * a price is an index, not a search key, so both adding a level and finding
 * the best one are array operations rather than a tree allocation and a tree
 * descent. That requires a bounded, pre-configured price range; see the
 * two-argument constructor.
 */
class OrderBook {
	public:
		/** Number of representable price levels used by the default constructor. */
		static constexpr std::size_t DefaultLevelCount = 65536;

		/**
		 * @brief Creates an order book with a small default price range.
		 *
		 * The default range and tick (integer prices 1 through 65536, tick 1)
		 * exist for tests and examples that do not care about the ladder's
		 * configuration. A real instrument should use the explicit
		 * constructor, sized to its actual tick size and trading range, and
		 * call it once at startup: the ladder never grows after construction.
		 */
		OrderBook();

		/**
		 * @brief Creates an order book with an explicitly sized price ladder.
		 * @param minPrice Lowest representable price on either side.
		 * @param tickSize Price increment between adjacent levels.
		 * @param levelCount Number of representable price levels.
		 * @throws std::invalid_argument for an invalid or overflowing range.
		 */
		OrderBook(Price minPrice, Price tickSize, std::size_t levelCount);

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
		 * @brief Shrinks a resting order in place, preserving its priority.
		 *
		 * The order keeps its price, its FIFO position, and its sequence
		 * number; only its remaining quantity and the level aggregate change.
		 * This is the sole modification that does not forfeit time priority,
		 * which is why repricing and size increases are expressed as a cancel
		 * followed by a new order rather than handled here.
		 *
		 * A missing order is reported rather than thrown: a replayed feed may
		 * reference an order that was resting before the captured window began.
		 *
		 * @return false when the order is absent; true after the reduction.
		 * @throws std::invalid_argument if newQuantity is zero or above the
		 * order's current remaining quantity.
		 */
		bool reduceOrder(const OrderId& orderId, Quantity newQuantity);

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

		/** Returns the number of occupied bid price levels. */
		std::size_t getBidLevelCount() const { return bids.occupiedCount(); }
		/** Returns the number of occupied ask price levels. */
		std::size_t getAskLevelCount() const { return asks.occupiedCount(); }

		/**
		 * @brief Pre-allocates order storage and index capacity for up to
		 * @p orderCount live orders. See OrderPool::reserve(),
		 * OrderIndex::reserve(), and ARCH_DECISIONS.md ADR-008. Call once,
		 * before trading begins.
		 */
		void reserveOrderCapacity(std::size_t orderCount);

	private:
		OrderPool orderPool;
		OrderIndex orderIndex;
		// Bids are indexed with descending=true, so "best" is the highest
		// price; asks are ascending, so "best" is the lowest.
		PriceLadder bids;
		PriceLadder asks;

		Order* addOrderWithQuantities(
			OrderId orderId, Price price, Quantity originalQuantity,
			Quantity remainingQuantity, Timestamp timestamp, OrderSide orderSide,
			OrderType orderType, SequenceNumber sequenceNumber);
};

} // namespace lob

#endif // ORDER_BOOK_HPP
