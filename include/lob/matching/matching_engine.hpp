#ifndef MATCHING_ENGINE_HPP
#define MATCHING_ENGINE_HPP

#include "lob/book/order_book.hpp"
#include "lob/matching/execution.hpp"
#include "lob/matching/sequence_number_generator.hpp"

#include <vector>

namespace lob {

class MatchingEngine {
	public:
		explicit MatchingEngine(OrderBook& orderBook);
		MatchingEngine(const MatchingEngine&) = delete;
		MatchingEngine& operator=(const MatchingEngine&) = delete;

		std::vector<Execution> processOrder(
			OrderId orderId,
			Price price,
			Quantity quantity,
			Timestamp timestamp,
			OrderSide orderSide,
			OrderType orderType);

	private:
		OrderBook& orderBook;
		SequenceNumberGenerator sequenceNumberGenerator;
};

} // namespace lob

#endif // MATCHING_ENGINE_HPP
