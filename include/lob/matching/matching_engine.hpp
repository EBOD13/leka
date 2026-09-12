#ifndef MATCHING_ENGINE_HPP
#define MATCHING_ENGINE_HPP

#include "lob/book/order_book.hpp"
#include "lob/matching/execution.hpp"
#include "lob/matching/sequence_number_generator.hpp"
#include "lob/order/order_event.hpp"

#include <vector>

namespace lob {

/**
 * @brief Matches incoming orders against resting liquidity in an OrderBook.
 *
 * The engine owns matching decisions while OrderBook owns resting order
 * storage and index consistency. Market orders are processed here rather
 * than inserted into the resting book.
 */
class MatchingEngine {
	public:
		/** Creates an engine that operates on the supplied book. */
		explicit MatchingEngine(OrderBook& orderBook);
		MatchingEngine(const MatchingEngine&) = delete;
		MatchingEngine& operator=(const MatchingEngine&) = delete;

		/**
		 * @brief Processes an incoming order and returns generated executions.
		 *
		 * The engine repeatedly examines the best opposing price level and its
		 * FIFO head. Limit orders cross only at prices permitted by their limit;
		 * market orders cross any available opposing liquidity. Each execution is
		 * priced from the resting order, preserving price-time priority.
		 *
		 * A remaining limit quantity is added to the book with its original
		 * submitted quantity preserved. Fully executed orders and unfilled market
		 * remainders are not stored in the book.
		 *
		 * @param orderId Unique identifier for the incoming order.
		 * @param price Limit price. Ignored for market orders.
		 * @param quantity Original quantity submitted by the caller.
		 * @param timestamp Timestamp assigned to the incoming order.
		 * @param orderSide Whether the order buys or sells.
		 * @param orderType Whether the order is a limit or market order.
		 * @return Executions in the order they occurred.
		 * @throws std::invalid_argument if an input value is invalid.
		 * @throws std::logic_error if the order ID is already present.
		 * @throws std::overflow_error if sequence numbers are exhausted.
		 *
		 * @details This convenience form returns a fresh vector on every call,
		 * so it allocates on the first execution just as returning any
		 * non-empty vector by value would. It exists for tests, tools, and
		 * call sites that do not run repeatedly on a latency-sensitive path.
		 * A caller in that position should use the buffer-taking overload
		 * below instead, reusing one vector across calls.
		 */
		std::vector<Execution> processOrder(
			OrderId orderId,
			Price price,
			Quantity quantity,
			Timestamp timestamp,
			OrderSide orderSide,
			OrderType orderType);

		/**
		 * @brief Same as above, but appends into a caller-owned buffer.
		 *
		 * @c out is cleared before matching begins, then filled in the order
		 * executions occurred. Its capacity is otherwise left alone: a caller
		 * that reuses the same vector across many calls pays for at most one
		 * allocation, ever, once that vector's capacity has grown to cover
		 * the largest execution burst seen so far. This is the form to use on
		 * a repeatedly-called, latency-sensitive path.
		 *
		 * @return The number of executions appended, equal to @c out.size().
		 */
		std::size_t processOrder(
			OrderId orderId,
			Price price,
			Quantity quantity,
			Timestamp timestamp,
			OrderSide orderSide,
			OrderType orderType,
			std::vector<Execution>& out);

		/**
		 * @brief Dispatches an event before interpreting its payload.
		 *
		 * NEW is the only event that can execute, so it is also the only event
		 * that consumes a sequence number. CANCEL and REDUCE return no
		 * executions and cannot cross the book, because neither can move an
		 * order to a price on the opposite side. A reprice is submitted as
		 * CANCEL followed by NEW, which routes it through the matcher and so
		 * cannot leave the book crossed.
		 *
		 * @details See processOrder() above for the same allocation tradeoff:
		 * this by-value form is a convenience wrapper, not the hot-path API.
		 */
		std::vector<Execution> processEvent(const OrderEvent& event);
		/** @brief Event-oriented alias for processEvent(). */
		std::vector<Execution> processOrder(const OrderEvent& event);

		/**
		 * @brief Buffer-taking form of processEvent(); see processOrder() above.
		 * @return The number of executions appended, equal to @c out.size().
		 */
		std::size_t processEvent(const OrderEvent& event, std::vector<Execution>& out);

	private:
		OrderBook& orderBook;
		SequenceNumberGenerator sequenceNumberGenerator;

		/**
		 * @brief The matching loop shared by every entry point above.
		 *
		 * Appends to @c out without clearing it first, so the two public
		 * overloads control clearing and this stays a single source of truth
		 * for the matching algorithm regardless of which entry point is used.
		 */
		void matchOrder(
			OrderId orderId,
			Price price,
			Quantity quantity,
			Timestamp timestamp,
			OrderSide orderSide,
			OrderType orderType,
			std::vector<Execution>& out);
};

} // namespace lob

#endif // MATCHING_ENGINE_HPP
