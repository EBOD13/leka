// include/lob/book/order_pool.hpp
#ifndef ORDER_POOL_HPP
#define ORDER_POOL_HPP

#include "lob/order/order.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lob {

/**
 * @brief Provides stable, pooled storage for Order objects.
 *
 * Storage is allocated in fixed 64 KiB pages. Orders are constructed in
 * place, never compacted, and released slots are reused without moving live
 * orders.
 */
class OrderPool {
    public:
        static constexpr std::size_t PageSize = 64 * 1024;
        static constexpr std::size_t SlotsPerPage = PageSize / sizeof(Order);

        /** Creates an empty pool without allocating a page. */
        OrderPool();
        OrderPool(OrderPool&&) = delete;
        OrderPool& operator=(OrderPool&&) = delete;
        OrderPool(const OrderPool&) = delete; // Delete copy constructor to prevent copying
        OrderPool& operator=(const OrderPool&) = delete; // Delete copy assignment operator to prevent copying
        /** Destroys all live orders and releases every allocated page. */
        ~OrderPool();

        /**
         * @brief Constructs an Order in a stable pool slot.
         * @return A pointer owned by this pool until release() is called.
         */
        Order* allocate(OrderId orderId, Price price, Quantity originalQuantity,
                        Timestamp timestamp, OrderSide orderSide,
                        OrderType orderType, SequenceNumber sequenceNumber);
        /** Constructs an Order with explicit original and remaining quantities. */
        Order* allocate(OrderId orderId, Price price, Quantity originalQuantity,
                Quantity remainingQuantity, Timestamp timestamp,
                OrderSide orderSide, OrderType orderType,
                SequenceNumber sequenceNumber);
        /** Destroys an order and returns its slot to the free list. */
        void release(Order* order);

        /**
         * @brief Pre-allocates enough pages to hold at least @p orderCount
         * live orders without allocate() creating a page on its own.
         *
         * A page is created lazily, on the first allocate() call that finds
         * no free slot, via mmap — a syscall with a genuinely unbounded tail
         * (page fault handling, kernel scheduling), which is why it shows up
         * as the multi-order-of-magnitude outliers in a latency histogram
         * (see ARCH_DECISIONS.md ADR-008). Calling this once, before trading
         * begins, for a known or comfortably over-estimated maximum order
         * count moves every one of those mmap calls out of the hot path
         * entirely, the same trade PriceLadder already makes for price
         * levels: pay a bounded, known cost once, in exchange for a flat
         * tail afterward. Exceeding @p orderCount still falls back to the
         * lazy per-allocation behavior rather than failing.
         */
        void reserve(std::size_t orderCount);

        /** Returns the number of allocated pages. */
        std::size_t getPageCount() const { return pages.size(); }
        /** Returns the number of live orders in the pool. */
        std::size_t getLiveOrderCount() const { return liveOrderCount; }

    private:
        struct Page;

        std::vector<std::unique_ptr<Page>> pages; // Vector to hold all allocated pages
        std::unordered_map<std::uintptr_t, Page*> pageIndex; // Map to quickly find the page for a given order pointer
        Page* firstPageWithFreeSlot{nullptr}; // Pointer to the first page that has at least one free slot
        std::size_t liveOrderCount{0}; // Count of currently allocated orders

        Page* createPage(); // Create a new page and add it to the pool
        void addToFreePageList(Page* page); // Add a page to the list of pages with free slots
        void removeFromFreePageList(Page* page); // Remove a page from the list of pages with free slots
};

static_assert(OrderPool::SlotsPerPage == 819); // Ensure that the number of slots per page is as expected

} // namespace lob

#endif // ORDER_POOL_HPP