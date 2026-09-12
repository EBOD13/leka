// include/lob/concurrency/spsc_event_queue.hpp
#ifndef SPSC_EVENT_QUEUE_HPP
#define SPSC_EVENT_QUEUE_HPP

#include "lob/order/order_event.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>

namespace lob {

/**
 * @brief Wait-free single-producer/single-consumer queue of OrderEvent.
 *
 * This is the boundary between a feed-handler thread (decoding or generating
 * events) and the matching thread (calling MatchingEngine::processEvent),
 * mirroring the two-thread split a real venue-facing system uses instead of
 * doing both jobs on one thread. See ARCH_DECISIONS.md ADR-009 for why this
 * shape, and measured cross-thread hand-off latency.
 *
 * Producer and consumer must each be called from exactly one thread apiece,
 * always the SAME thread for the lifetime of the queue — tryPush() from more
 * than one thread, or tryPop() from more than one thread, is undefined
 * behavior. That restriction is what makes this SPSC rather than MPMC, and
 * is also what makes it possible to implement with no compare-and-swap loop
 * at all: each side has exactly one writer for its own cursor, so a plain
 * atomic store, correctly ordered, is enough. No lock, no CAS retry loop,
 * and no blocking: tryPush()/tryPop() either succeed immediately or report
 * "not right now" and return, which is what "wait-free" means here.
 *
 * head_ and tail_ are each on their own 64-byte cache line
 * (std::hardware_destructive_interference_size on most platforms actually
 * targeted, but the exact value is not guaranteed portable, so this uses the
 * conventional 64 directly). Without that padding, the producer's tail_
 * store and the consumer's head_ store would share a cache line, and every
 * single push and pop would force that line to bounce between the two
 * cores' caches (MESI/MOESI invalidation) even though the two threads never
 * touch the same logical field — "false sharing," and a real, measurable
 * throughput cost specifically because there IS no lock here to hide it
 * behind.
 *
 * Storage is raw, placement-constructed bytes rather than
 * std::array<OrderEvent, Capacity>, the same technique OrderPool
 * (include/lob/book/order_pool.hpp) already uses for Order: OrderEvent has
 * no default constructor by design (see order_event.hpp), and a
 * std::array of it would require one.
 */
template <std::size_t Capacity>
class SpscEventQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

    public:
        SpscEventQueue() = default;
        SpscEventQueue(const SpscEventQueue&) = delete;
        SpscEventQueue& operator=(const SpscEventQueue&) = delete;
        SpscEventQueue(SpscEventQueue&&) = delete;
        SpscEventQueue& operator=(SpscEventQueue&&) = delete;

        /** Destroys any events left in the queue at the time of destruction. */
        ~SpscEventQueue() {
            std::size_t head = head_.load(std::memory_order_relaxed);
            const std::size_t tail = tail_.load(std::memory_order_relaxed);
            for (; head != tail; ++head) {
                std::destroy_at(slot(head));
            }
        }

        /**
         * @brief Producer-only. Appends @p event if the queue is not full.
         * @return false if the queue was full; @p event is left untouched.
         */
        bool tryPush(const OrderEvent& event) {
            const std::size_t tail = tail_.load(std::memory_order_relaxed);
            if (tail - cachedHead_ >= Capacity) {
                // Only re-read the consumer's cursor, an inter-core load,
                // when our own stale copy says we might be full. Once
                // refreshed, that copy is good for every push up to the
                // point it was taken, so a producer draining into a
                // consumer that is comfortably keeping up almost never
                // pays this load at all.
                cachedHead_ = head_.load(std::memory_order_acquire);
                if (tail - cachedHead_ >= Capacity) {
                    return false; // genuinely full
                }
            }
            std::construct_at(slot(tail), event);
            // release: publishes both the new tail AND everything written
            // above it (the constructed OrderEvent) to the consumer's
            // subsequent acquire load of tail_.
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        /**
         * @brief Consumer-only. Moves the next event into @p out if present.
         * @return false if the queue was empty; @p out is left untouched.
         */
        bool tryPop(OrderEvent& out) {
            const std::size_t head = head_.load(std::memory_order_relaxed);
            if (head == cachedTail_) {
                cachedTail_ = tail_.load(std::memory_order_acquire);
                if (head == cachedTail_) {
                    return false; // genuinely empty
                }
            }
            OrderEvent* eventSlot = slot(head);
            out = std::move(*eventSlot);
            std::destroy_at(eventSlot);
            // release: publishes the freed slot to the producer's
            // subsequent acquire load of head_ in tryPush()'s refresh path.
            head_.store(head + 1, std::memory_order_release);
            return true;
        }

        /**
         * @brief Approximate occupancy, for diagnostics only.
         *
         * Reads both cursors without synchronizing them against each other,
         * so the result can be stale or transiently negative-looking
         * (wrapped) the instant either side is mid-operation. Never use
         * this to decide whether tryPush()/tryPop() will succeed; call them
         * and check their return value instead.
         */
        std::size_t sizeApprox() const {
            return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
        }

        static constexpr std::size_t capacity() { return Capacity; }

    private:
        static constexpr std::size_t mask_ = Capacity - 1;

        OrderEvent* slot(std::size_t index) {
            return std::launder(reinterpret_cast<OrderEvent*>(
                &storage_[(index & mask_) * sizeof(OrderEvent)]));
        }

        // Every member below is deliberately on its own cache line. head_
        // is written only by the consumer and read by the producer; tail_
        // is written only by the producer and read by the consumer;
        // cachedHead_ and cachedTail_ are each private to one side and
        // never touched by the other thread at all, but still get their
        // own line so that a write to one never invalidates a neighbor a
        // future change might otherwise pack in beside it.
        alignas(64) std::atomic<std::size_t> head_{0};
        alignas(64) std::atomic<std::size_t> tail_{0};
        alignas(64) std::size_t cachedHead_{0}; // producer-private view of head_
        alignas(64) std::size_t cachedTail_{0}; // consumer-private view of tail_
        alignas(alignof(OrderEvent)) std::array<std::byte, Capacity * sizeof(OrderEvent)> storage_;
};

} // namespace lob

#endif // SPSC_EVENT_QUEUE_HPP
