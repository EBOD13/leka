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
		 */
		std::vector<Execution> processOrder(
			OrderId orderId,
			Price price,
			Quantity quantity,
			Timestamp timestamp,
			OrderSide orderSide,
			OrderType orderType);

		/**
		 * @brief Dispatches an event before interpreting its payload.
		 *
		 * CANCEL and MODIFY events return no executions. MODIFY allocates a new
		 * sequence number only when the change resets price/time priority.
		 */
		std::vector<Execution> processEvent(const OrderEvent& event);
		/** @brief Event-oriented alias for processEvent(). */
		std::vector<Execution> processOrder(const OrderEvent& event);

	private:
		OrderBook& orderBook;
		SequenceNumberGenerator sequenceNumberGenerator;
};

} // namespace lob

#endif // MATCHING_ENGINE_HPP
