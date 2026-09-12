// benchmarks/benchmark_main.cpp

/**
 * @file
 * @brief Percentile latency benchmark for MatchingEngine::processEvent.
 *
 * The original version of this file measured one mean over 100,000
 * add-then-cancel operations at a single price level. That is not
 * representative of a live book (real depth spans thousands of price levels,
 * and a mean hides exactly the tail that matters for a latency-sensitive
 * system) and it could not distinguish the two data structures compared in
 * ARCH_DECISIONS.md: with one level occupied, a red-black tree node and an
 * array slot cost about the same to reach.
 *
 * This version builds a book with resting liquidity spread across many price
 * levels, then times a mixed, seeded-random stream of resting adds, crossing
 * (marketable) adds, cancels, and best-of-book queries, and reports p50 / p90
 * / p99 / p99.9 / max per category rather than a single average.
 */

#include "lob/matching/matching_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/** @brief Collects nanosecond samples per label and reports them as percentiles. */
class LatencyLog {
    public:
        void record(const char *label, std::uint64_t nanos) {
            samples[label].push_back(nanos);
        }

        void report() const {
            for (auto &[label, values] : samples) {
                if (values.empty()) {
                    continue;
                }
                std::vector<std::uint64_t> sorted = values;
                std::sort(sorted.begin(), sorted.end());
                const auto pct = [&](double p) {
                    const std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1));
                    return sorted[idx];
                };
                std::printf(
                    "  %-10s n=%-8zu p50=%-6llu p90=%-6llu p99=%-6llu p99.9=%-6llu max=%llu (ns)\n",
                    label.c_str(), sorted.size(),
                    static_cast<unsigned long long>(pct(0.50)),
                    static_cast<unsigned long long>(pct(0.90)),
                    static_cast<unsigned long long>(pct(0.99)),
                    static_cast<unsigned long long>(pct(0.999)),
                    static_cast<unsigned long long>(sorted.back()));
            }
        }

    private:
        std::unordered_map<std::string, std::vector<std::uint64_t>> samples;
};

template <typename Fn>
void timed(LatencyLog &log, const char *label, Fn &&fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    log.record(label, static_cast<std::uint64_t>(elapsed));
}

} // namespace

int main() {
    using namespace lob;

    // Bids occupy [1, Levels]; asks occupy [Levels+1, 2*Levels]. Both ladders
    // are configured to cover the full [1, 2*Levels] range, as OrderBook
    // requires (see ARCH_DECISIONS.md for why the range must be fixed up
    // front), even though each side only ever uses its own half.
    constexpr std::uint64_t Levels = 2000;
    constexpr std::size_t LevelCount = 2 * Levels + 1;
    constexpr std::uint64_t WarmupOrdersPerSide = Levels; // one resting order per level
    constexpr std::uint64_t TimedIterations = 300000;

    OrderBook book{Price{1}, Price{1}, LevelCount};
    // Sized generously above the true worst case (warmup plus every timed
    // iteration resting, which none of the mix ever reaches in practice) so
    // OrderPool and OrderIndex never allocate or rehash once trading starts.
    // See ARCH_DECISIONS.md ADR-008: without this, OrderIndex's occasional
    // doubling rehash was the actual source of NEW_REST's multi-order-of-
    // magnitude max latency, not OrderPool's page allocation as first
    // suspected.
    book.reserveOrderCapacity(WarmupOrdersPerSide * 2 + TimedIterations);
    MatchingEngine engine{book};
    const Timestamp timestamp{1};

    std::mt19937_64 rng{42}; // fixed seed: reproducible across runs and machines
    std::uniform_int_distribution<std::uint64_t> bidPrice(1, Levels);
    std::uniform_int_distribution<std::uint64_t> askPrice(Levels + 1, 2 * Levels);
    std::uniform_int_distribution<int> action(0, 99);

    std::uint64_t nextOrderId = 1;
    std::vector<OrderId> live;
    live.reserve(WarmupOrdersPerSide * 2 + TimedIterations);

    // Warm-up (untimed): seed one resting order per level on each side so the
    // benchmark starts from realistic depth rather than an empty book.
    for (std::uint64_t i = 0; i < WarmupOrdersPerSide; ++i) {
        const OrderId bidId{nextOrderId++};
        engine.processOrder(bidId, Price{bidPrice(rng)}, Quantity{10}, timestamp,
                             OrderSide::BUY, OrderType::LIMIT);
        live.push_back(bidId);

        const OrderId askId{nextOrderId++};
        engine.processOrder(askId, Price{askPrice(rng)}, Quantity{10}, timestamp,
                             OrderSide::SELL, OrderType::LIMIT);
        live.push_back(askId);
    }

    LatencyLog latency;
    // Reused across every timed NEW below: MatchingEngine's buffer-taking
    // overload (see ARCH_DECISIONS.md) clears and refills this same vector
    // instead of returning a fresh one, so after its capacity stabilizes to
    // the largest execution burst seen, filling it costs no allocation at
    // all. Discarding the by-value return, as the previous version of this
    // benchmark did, still pays for a heap allocation on every single
    // execution — that allocation is exactly what this buffer removes.
    std::vector<Execution> executionBuffer;
    const auto benchmarkStart = std::chrono::steady_clock::now();

    // Timed phase: 55% resting NEW, 15% crossing (marketable) NEW, 25% CANCEL,
    // 5% best-of-book query, drawn from a fixed seed so the mix is identical
    // across runs.
    for (std::uint64_t i = 0; i < TimedIterations; ++i) {
        const int roll = action(rng);
        if (roll < 55) {
            const bool buy = (roll % 2 == 0);
            const OrderId id{nextOrderId++};
            const Price price{buy ? bidPrice(rng) : askPrice(rng)};
            timed(latency, "NEW_REST", [&] {
                engine.processOrder(id, price, Quantity{1}, timestamp,
                                     buy ? OrderSide::BUY : OrderSide::SELL,
                                     OrderType::LIMIT, executionBuffer);
            });
            live.push_back(id);
        } else if (roll < 70) {
            // Priced to cross whatever currently rests on the opposite side.
            const bool buy = (roll % 2 == 0);
            const OrderId id{nextOrderId++};
            const Price price{buy ? Price{2 * Levels} : Price{1}};
            timed(latency, "NEW_CROSS", [&] {
                engine.processOrder(id, price, Quantity{1}, timestamp,
                                     buy ? OrderSide::BUY : OrderSide::SELL,
                                     OrderType::LIMIT, executionBuffer);
            });
            // A crossing order may fully fill and never rest, so it is not
            // added to `live`; cancelling a nonexistent ID is a harmless
            // no-op (see OrderBook::cancelOrder), the same as it would be in
            // production if an order matched a moment before a cancel for it
            // arrived.
        } else if (roll < 95 && !live.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t index = pick(rng);
            const OrderId id = live[index];
            live[index] = live.back();
            live.pop_back();
            timed(latency, "CANCEL", [&] {
                engine.processEvent(OrderEvent{CancelOrder{id}});
            });
        } else {
            timed(latency, "BEST", [&] {
                volatile const PriceLevel *bestBid = book.getBestBid();
                volatile const PriceLevel *bestAsk = book.getBestAsk();
                (void)bestBid;
                (void)bestAsk;
            });
        }
    }

    const auto totalElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - benchmarkStart).count();

    std::printf("benchmark_order_book: %llu timed events over %.1f ms (%.0f events/sec)\n",
                static_cast<unsigned long long>(TimedIterations),
                static_cast<double>(totalElapsed) / 1e6,
                static_cast<double>(TimedIterations) / (static_cast<double>(totalElapsed) / 1e9));
    latency.report();
}
