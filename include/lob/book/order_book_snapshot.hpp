// include/lob/book/order_book_snapshot.hpp
#ifndef ORDER_BOOK_SNAPSHOT_HPP
#define ORDER_BOOK_SNAPSHOT_HPP

#include "lob/types/price.hpp"
#include "lob/types/quantity.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

class OrderBook;

/**
 * @file
 * @brief Point-in-time view of book state for consumers outside the engine.
 *
 * This is the single data model shared by every out-of-engine consumer: the
 * offline recorder that writes a replay to disk, and (later) a live server
 * publishing to a browser. Both produce the same fields, so a visualization
 * written against a recorded file works unchanged against a live feed.
 *
 * The engine never produces one of these on its own. A caller captures a
 * snapshot *between* events, never inside MatchingEngine::processEvent(), so
 * nothing here can appear on the matching hot path. See ARCH_DECISIONS.md
 * ADR-010 for the measurement proving that separation holds.
 */

/** @brief One aggregated price level as seen from outside the engine. */
struct BookSnapshotLevel {
    /** Price in raw integer units (1/10000, matching Price's own encoding). */
    std::uint64_t priceRaw = 0;
    /** Total resting quantity at this level. */
    std::uint64_t quantity = 0;
    /** Number of distinct resting orders at this level. */
    std::size_t orderCount = 0;
};

/**
 * @brief Top-of-book depth plus the counters a viewer needs for context.
 *
 * Levels are ordered best-first on each side: bids descending in price, asks
 * ascending. A side with no resting liquidity yields an empty vector and a
 * zero best price, zero being Price's own reserved invalid value.
 */
struct BookSnapshot {
    /** Number of events applied to the book before this snapshot. */
    std::uint64_t sequence = 0;
    /** Engine timestamp of the most recently applied event, in nanoseconds. */
    std::uint64_t tsNs = 0;

    /** Best bid price in raw units, or 0 when no bids rest. */
    std::uint64_t bestBidRaw = 0;
    /** Best ask price in raw units, or 0 when no asks rest. */
    std::uint64_t bestAskRaw = 0;

    /** Top levels, best first. */
    std::vector<BookSnapshotLevel> bids;
    std::vector<BookSnapshotLevel> asks;

    /** Occupied level count per side, which may exceed the captured depth. */
    std::size_t bidLevelCount = 0;
    std::size_t askLevelCount = 0;

    /** Clears the vectors without releasing their capacity, so a caller
     *  reusing one snapshot across captures stops allocating after warmup. */
    void reset() {
        bids.clear();
        asks.clear();
        sequence = 0;
        tsNs = 0;
        bestBidRaw = 0;
        bestAskRaw = 0;
        bidLevelCount = 0;
        askLevelCount = 0;
    }
};

} // namespace lob

#endif // ORDER_BOOK_SNAPSHOT_HPP
