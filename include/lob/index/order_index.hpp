//include/lob/index/order_index.hpp

#ifndef ORDER_INDEX_HPP
#define ORDER_INDEX_HPP
#include "lob/types/order_id.hpp"
#include "lob/order/order.hpp"
#include <cstddef>
#include <vector>

namespace lob {

/**
 * @brief Open-addressed OrderId -> Order* index with average O(1) lookup.
 *
 * OrderId::isValid() reserves zero, which lets an empty Slot's default
 * OrderId double as the "unoccupied" sentinel: no separate occupancy bitmap
 * or tombstone state is needed. Slots live in one contiguous std::vector, so
 * every element of the whole probe sequence for a lookup is one flat array,
 * not the one-heap-allocation-per-entry, pointer-chasing structure a chained
 * hash table (such as std::unordered_map) builds.
 *
 * This is called on every add, cancel, reduce, and fill, so it is the
 * hottest lookup in the engine. Unlike PriceLevel's PriceLadder, there is no
 * natural fixed bound on how many orders can be live at once, so this grows
 * by amortized doubling like std::vector rather than being pre-sized once.
 *
 * Removal uses backward-shift deletion rather than tombstones: on removal,
 * every following entry in the same probe run is shifted back into the
 * vacated slot if doing so keeps it reachable, and the search for the next
 * vacated slot continues until a genuinely empty one is found. This keeps
 * lookups a single clean scan for empty forever, with no growing tombstone
 * debt from a long session with many cancels, at the cost of removal being
 * more than a single slot write.
 */
class OrderIndex {
    public:
        /**
         * @brief Adds an order and rejects duplicate IDs.
         *
         * @details Only checks this index's own invariants: that @p order is
         * non-null and that its ID is not already present. It does not
         * re-check @p order's overall field validity (side, type, price,
         * quantity, timestamp) — that is OrderBook::addOrderWithQuantities's
         * job, and it always runs before an order reaches any book structure,
         * this index included. PriceLevel::addOrder follows the same
         * division: each class enforces only the invariant it alone owns,
         * rather than every class re-verifying the whole Order on every
         * insert.
         */
        void addOrder(Order* order);

        /** Removes an order after verifying pointer identity. */
        void removeOrder(Order* order);

        /** Finds an order by ID, or returns nullptr when absent. */
        Order* findOrder(const OrderId& orderId) const;

        /** Returns the number of indexed orders. */
        std::size_t size() const { return count; }

        /**
         * @brief Grows the table, if needed, so @p orderCount entries fit
         * under MaxLoadFactor without a later doubling.
         *
         * Unlike PriceLadder, this table has no fixed bound to pre-size to
         * exactly, so it still grows on demand for anyone who does not call
         * this first (see the class comment). But a table that doubles while
         * holding many live entries pays for an O(n) rehash of everything
         * still indexed at the moment it happens, not the O(1) amortized cost
         * the growth policy implies on average — a single one of those, late
         * in a run with a large book, is a real tail-latency event (see
         * ARCH_DECISIONS.md ADR-008). Calling this once, for a known or
         * comfortably over-estimated order count, moves that rehash out of
         * the hot path the same way OrderPool::reserve() moves page creation
         * out of it.
         */
        void reserve(std::size_t orderCount);

    private:
        /** One slot; an OrderId{} key (0, reserved invalid) means empty. */
        struct Slot {
            OrderId key;
            Order* value{nullptr};
        };

        static constexpr std::size_t InitialCapacity = 16; // must stay a power of two
        static constexpr double MaxLoadFactor = 0.5;

        std::vector<Slot> slots;
        std::size_t count{0};

        std::size_t indexFor(const OrderId& id, std::size_t capacity) const;
        std::size_t findSlot(const OrderId& id) const;
        void growIfNeeded();
};

} // namespace lob
#endif // ORDER_INDEX_HPP
