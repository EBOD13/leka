// tests/stress/test_spsc_queue.cpp

/**
 * @file
 * @brief Real two-thread correctness test for SpscEventQueue.
 *
 * A single-threaded test cannot expose a concurrency bug: the whole class of
 * failure this queue could have (memory ordering wrong, cached-cursor
 * refresh wrong, false sharing that only shows up under real contention)
 * requires two real OS threads actually racing against the same instance.
 * A small capacity relative to the event count forces many full/empty
 * transitions and many wraparounds, which is exactly where a subtle
 * cached-cursor bug would show up and a single-shot small test would not
 * reach.
 *
 * Build this target with -DENABLE_TSAN=ON (see cmake/Sanitizers.cmake) for
 * ThreadSanitizer coverage in addition to what this test checks by
 * construction; ASan/UBSan and TSan cannot run in the same binary.
 */

#include "lob/concurrency/spsc_event_queue.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

int main() {
	// Deliberately much smaller than the event count: capacity 1024 against
	// 2,000,000 events forces roughly 2,000 full wraparounds.
	constexpr std::size_t Capacity = 1024;
	constexpr std::uint64_t EventCount = 2'000'000;

	lob::SpscEventQueue<Capacity> queue;
	const lob::Timestamp timestamp{1};

	std::thread producer([&] {
		for (std::uint64_t i = 1; i <= EventCount; ++i) {
			// CancelOrder carries one field, orderId, which doubles here as
			// the sequence number the consumer checks: any reordering,
			// drop, duplication, or corruption shows up as this sequence
			// breaking.
			const lob::OrderEvent event{lob::CancelOrder{lob::OrderId{i}}};
			while (!queue.tryPush(event)) {
				std::this_thread::yield(); // queue full; consumer will drain it
			}
		}
	});

	std::vector<std::uint64_t> received;
	received.reserve(EventCount);
	std::uint64_t consumed = 0;
	while (consumed < EventCount) {
		lob::OrderEvent event{lob::CancelOrder{lob::OrderId{}}}; // placeholder, overwritten by tryPop
		if (queue.tryPop(event)) {
			received.push_back(event.getCancelOrder().orderId.getId());
			++consumed;
		} else {
			std::this_thread::yield(); // queue empty; producer will refill it
		}
	}

	producer.join();

	// Exact FIFO order, no loss, no duplication, no corruption: received
	// must be the identity sequence 1..EventCount, in that exact order.
	assert(received.size() == EventCount);
	for (std::uint64_t i = 0; i < EventCount; ++i) {
		assert(received[i] == i + 1);
	}

	// The queue must have fully drained: nothing left for the destructor to
	// clean up, and a fresh push/pop pair must still work correctly at
	// whatever wrapped cursor position the run above left behind.
	assert(queue.sizeApprox() == 0);
	lob::OrderEvent probe{lob::CancelOrder{lob::OrderId{}}};
	assert(!queue.tryPop(probe)); // genuinely empty, not a stale false-empty report

	const lob::OrderEvent finalEvent{lob::NewOrder{
		lob::OrderId{999999}, lob::Price{100}, lob::Quantity{1}, timestamp,
		lob::OrderSide::BUY, lob::OrderType::LIMIT}};
	assert(queue.tryPush(finalEvent));
	assert(queue.tryPop(probe));
	assert(probe.getEventType() == lob::OrderEventType::NEW);
	assert(probe.getNewOrder().orderId == lob::OrderId{999999});

	return 0;
}
