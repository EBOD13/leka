// tests/stress/test_randomized_operations.cpp

/**
 * @file
 * @brief Differential stress test for OrderIndex's open-addressed table.
 *
 * OrderIndex replaced std::unordered_map<OrderId, Order*> with a hand-rolled
 * open-addressed table using backward-shift deletion (see ARCH_DECISIONS.md).
 * Insertion is easy to get right; deletion is not — a single incorrect
 * backward-shift can silently strand an unrelated, still-live key behind a
 * hole, so that key becomes permanently unreachable even though it was never
 * touched. That failure mode does not show up from a handful of hand-picked
 * unit test cases: it needs many entries, many interleaved removals, and a
 * table that has grown at least once, so it is exercised here with a
 * randomized differential test against std::unordered_map as an oracle.
 *
 * A fixed seed keeps this reproducible. The full live set is re-verified
 * against the oracle after every single operation rather than at the end,
 * so a failure points at the exact operation that broke invariants instead
 * of an accumulated, hard-to-localize symptom.
 */

#include "lob/index/order_index.hpp"

#include <cassert>
#include <cstdint>
#include <iterator>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

int main() {
	lob::OrderIndex index;
	std::unordered_map<std::uint64_t, lob::Order*> oracle;
	std::vector<std::unique_ptr<lob::Order>> storage; // keeps every Order's address stable

	std::mt19937_64 rng{1234567}; // fixed seed: reproducible failures
	std::uniform_int_distribution<int> chooseAction(0, 1); // 0 = add, 1 = remove
	// Deliberately small relative to the iteration count so IDs collide
	// repeatedly under the table's mask, including across a growth step.
	std::uniform_int_distribution<std::uint64_t> idPool(1, 4000);

	const auto verifyAgainstOracle = [&]() {
		assert(index.size() == oracle.size());
		for (const auto& [id, order] : oracle) {
			assert(index.findOrder(lob::OrderId{id}) == order);
		}
	};

	constexpr int Iterations = 20000;
	std::uint64_t nextSequence = 1;
	for (int i = 0; i < Iterations; ++i) {
		const bool shouldAdd = oracle.empty() || chooseAction(rng) == 0;
		if (shouldAdd) {
			std::uint64_t id;
			do {
				id = idPool(rng);
			} while (id == 0 || oracle.count(id) != 0);

			storage.push_back(std::make_unique<lob::Order>(
				lob::OrderId{id}, lob::Price{100}, lob::Quantity{10},
				lob::Timestamp{1}, lob::OrderSide::BUY, lob::OrderType::LIMIT,
				lob::SequenceNumber{nextSequence++}));
			lob::Order* order = storage.back().get();

			index.addOrder(order);
			oracle.emplace(id, order);
		} else {
			auto it = oracle.begin();
			std::advance(it, std::uniform_int_distribution<std::size_t>(
				0, oracle.size() - 1)(rng));
			lob::Order* order = it->second;

			index.removeOrder(order);
			oracle.erase(it);
		}
		verifyAgainstOracle();
	}

	// Drain everything through removeOrder and confirm the index empties
	// cleanly rather than leaking stale slots.
	for (const auto& [id, order] : oracle) {
		index.removeOrder(order);
	}
	assert(index.size() == 0);
	assert(index.findOrder(lob::OrderId{1}) == nullptr);

	return 0;
}
