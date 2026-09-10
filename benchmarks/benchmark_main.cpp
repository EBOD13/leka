#include "lob/matching/matching_engine.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
	lob::OrderBook book;
	lob::MatchingEngine engine{book};
	const lob::Timestamp timestamp{1};
	constexpr std::uint64_t iterations = 100000;

	const auto start = std::chrono::steady_clock::now();
	for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
		const auto orderId = lob::OrderId{iteration + 1};
		engine.processOrder(orderId, lob::Price{100}, lob::Quantity{1},
							 timestamp, lob::OrderSide::BUY,
							 lob::OrderType::LIMIT);
		book.cancelOrder(orderId);
	}
	const auto finish = std::chrono::steady_clock::now();
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
		finish - start).count();

	std::cout << "iterations=" << iterations
			  << " total_ns=" << elapsed
			  << " ns_per_order=" << static_cast<double>(elapsed) / iterations
			  << '\n';
}
