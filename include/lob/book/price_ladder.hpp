// include/lob/book/price_ladder.hpp
#ifndef PRICE_LADDER_HPP
#define PRICE_LADDER_HPP

#include "lob/book/price_level.hpp"
#include "lob/types/price.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

/**
 * @brief Fixed-range, tick-indexed array of PriceLevel slots for one side of a book.
 *
 * A price is not treated as an ordered-map key; it is treated as an index.
 * Every level in [minPrice, minPrice + (levelCount-1)*tickSize] is constructed
 * once, at PriceLadder construction, and never destroyed or moved for the
 * ladder's lifetime. Adding an order at a price that already has resting
 * orders, or that has never had one, is the same O(1) array access with no
 * allocation and no tree rebalancing; the old std::map<Price, PriceLevel>
 * design paid a node allocation for the former and a pointer-chasing tree
 * descent for the latter on every best-of-book query.
 *
 * Because PriceLevel addresses are stable for the ladder's lifetime, Order's
 * cached PriceLevel* pointer stays valid across every insert and remove at
 * any other price: nothing is ever reallocated out from under it.
 *
 * "Best" is always the lowest occupied index. Index 0 is the worst price a
 * resting order can have on that side, so the two sides differ only in which
 * direction price increases with index: ascending for asks (index 0 is the
 * lowest, hence best, ask) and descending for bids (index 0 is the highest,
 * hence best, bid). This lets both sides share one implementation and one
 * "lowest set bit" query instead of a min-heap on one side and a max-heap on
 * the other.
 *
 * Occupancy is tracked in a bitset rather than by asking each PriceLevel
 * whether it is empty, so finding the best level after it empties is a
 * hardware find-first-set over a handful of 64-bit words rather than a linear
 * scan of PriceLevel objects. The best index is additionally cached, so the
 * common case, a level away from the current best changing occupancy, costs
 * one array access and one bit flip with no search at all.
 */
class PriceLadder {
    public:
        /** Sentinel returned when no level is occupied. */
        static constexpr std::size_t npos = static_cast<std::size_t>(-1);

        /**
         * @brief Pre-allocates every level the ladder will ever hold.
         *
         * @param minPrice Lowest representable price on this side.
         * @param tickSize Price increment between adjacent indices.
         * @param levelCount Number of representable price levels.
         * @param descending True for the bid side, where index 0 is the
         * highest price rather than the lowest.
         * @throws std::invalid_argument if minPrice is zero, tickSize is
         * zero, levelCount is zero, or the configured range overflows.
         */
        PriceLadder(Price minPrice, Price tickSize, std::size_t levelCount,
                    bool descending);

        PriceLadder(const PriceLadder&) = delete;
        PriceLadder& operator=(const PriceLadder&) = delete;
        PriceLadder(PriceLadder&&) = delete;
        PriceLadder& operator=(PriceLadder&&) = delete;
        ~PriceLadder() = default;

        /**
         * @brief Returns the level slot for a price; the slot always exists.
         * @throws std::out_of_range if the price falls outside the
         * configured range.
         * @throws std::invalid_argument if the price does not fall on a
         * configured tick boundary.
         */
        PriceLevel& levelAt(Price price);

        /**
         * @brief Marks a level occupied; call exactly once when its order
         * count transitions from zero to nonzero.
         */
        void markOccupied(Price price);

        /**
         * @brief Marks a level empty; call exactly once when its order count
         * transitions from nonzero to zero.
         */
        void markEmpty(Price price);

        /** Returns the best occupied level, or nullptr when the side is empty. */
        PriceLevel* best();
        /** Returns the best occupied level for read-only inspection. */
        const PriceLevel* best() const;

        /** Returns the number of currently occupied levels. */
        std::size_t occupiedCount() const { return occupied; }

        /**
         * @brief Walks up to @p maxLevels occupied levels, best first.
         *
         * Read-only traversal for market-data snapshots. Because "best" is
         * always the lowest occupied index on either side (see the class
         * comment), walking outward from the touch is the same forward
         * bit-scan on both bids and asks, and it visits levels in true
         * price priority order without sorting anything.
         *
         * This deliberately stops after @p maxLevels rather than walking the
         * whole ladder: a snapshot consumer wants the top of book, and the
         * ladder may span hundreds of thousands of mostly-empty levels.
         */
        template <typename Fn>
            requires std::invocable<Fn&, const PriceLevel&>
        void forEachOccupied(std::size_t maxLevels, Fn&& fn) const {
            std::size_t index = bestIndex;
            for (std::size_t seen = 0; seen < maxLevels && index != npos; ++seen) {
                fn(levels[index]);
                index = nextSetBit(index + 1);
            }
        }

    private:
        std::size_t indexOf(Price price) const;
        std::size_t nextSetBit(std::size_t from) const;

        std::uint64_t minPrice;
        std::uint64_t tickSize;
        std::uint64_t maxPrice; // minPrice + (levelCount - 1) * tickSize, cached at construction
        std::size_t levelCount;
        bool descending;

        // One entry per tick; constructed once and never resized, so every
        // PriceLevel's address is stable for the life of the ladder.
        std::vector<PriceLevel> levels;
        // Occupancy bitset, 64 levels per word.
        std::vector<std::uint64_t> words;
        // Lowest occupied index, kept exact; npos when occupied == 0.
        std::size_t bestIndex{npos};
        std::size_t occupied{0};
};

} // namespace lob

#endif // PRICE_LADDER_HPP
