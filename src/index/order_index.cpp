// src/index/order_index.cpp

#include "lob/index/order_index.hpp"
#include <functional>
#include <stdexcept>
#include <vector>

namespace lob {

namespace {

/**
 * @brief True when index k lies in the circular range (i, j], going forward
 * from i (exclusive) to j (inclusive), wrapping past capacity if i > j.
 *
 * This is the standard backward-shift-deletion test: an entry whose natural
 * hash index falls in this range still needs the probe run starting at i to
 * reach it, so it cannot be moved back into slot i without becoming
 * unreachable. An entry whose natural index falls outside this range no
 * longer needs slot i's occupant at all and can be pulled back into it.
 */
bool inCyclicRange(std::size_t k, std::size_t i, std::size_t j, std::size_t capacity) {
    (void)capacity;
    if (i <= j) {
        return i < k && k <= j;
    }
    return k > i || k <= j;
}

} // namespace

std::size_t OrderIndex::indexFor(const OrderId& id, std::size_t capacity) const {
    return std::hash<OrderId>{}(id) & (capacity - 1);
}

/**
 * @details Scans forward from the natural index until either a matching key
 * or a genuinely empty slot is found. Backward-shift deletion is what makes
 * "empty slot" a reliable stopping condition: without it, a naive removal
 * could leave a hole partway through another key's probe run and this scan
 * would wrongly report that key as absent.
 */
std::size_t OrderIndex::findSlot(const OrderId& id) const {
    if (slots.empty()) {
        return static_cast<std::size_t>(-1);
    }
    std::size_t index = indexFor(id, slots.size());
    while (slots[index].value != nullptr) {
        if (slots[index].key == id) {
            return index;
        }
        index = (index + 1) & (slots.size() - 1);
    }
    return index; // the empty slot where `id` would be inserted
}

/**
 * @details Shares the doubling-and-rehash body with growIfNeeded() by simply
 * picking the target capacity up front and looping the same "not big enough
 * yet, double again" test growIfNeeded() uses one step at a time. Never
 * shrinks: reserving a smaller count than the table already holds is a no-op.
 */
void OrderIndex::reserve(std::size_t orderCount) {
    std::size_t capacity = slots.empty() ? InitialCapacity : slots.size();
    while (static_cast<double>(orderCount) > static_cast<double>(capacity) * MaxLoadFactor) {
        capacity *= 2;
    }
    if (!slots.empty() && capacity <= slots.size()) {
        return;
    }

    std::vector<Slot> grown(capacity, Slot{});
    for (const Slot& slot : slots) {
        if (slot.value == nullptr) {
            continue;
        }
        std::size_t index = indexFor(slot.key, grown.size());
        while (grown[index].value != nullptr) {
            index = (index + 1) & (grown.size() - 1);
        }
        grown[index] = slot;
    }
    slots.swap(grown);
}

void OrderIndex::growIfNeeded() {
    if (slots.empty()) {
        slots.assign(InitialCapacity, Slot{});
        return;
    }
    if (static_cast<double>(count + 1) <= static_cast<double>(slots.size()) * MaxLoadFactor) {
        return;
    }
    std::vector<Slot> grown(slots.size() * 2, Slot{});
    for (const Slot& slot : slots) {
        if (slot.value == nullptr) {
            continue;
        }
        std::size_t index = indexFor(slot.key, grown.size());
        while (grown[index].value != nullptr) {
            index = (index + 1) & (grown.size() - 1);
        }
        grown[index] = slot;
    }
    slots.swap(grown);
}

/**
 * @details Does not call order->isValid(): OrderBook::addOrderWithQuantities
 * already does, before the order is linked into any book structure, so
 * repeating it here would re-check the same fields on every accepted insert
 * for an order this index cannot yet have any reason to distrust. See the
 * header for the full reasoning.
 */
void OrderIndex::addOrder(Order* order) {
    if (order == nullptr) {
        throw std::invalid_argument("Cannot add a null order");
    }

    // Grown before probing so the probe below operates on the table the
    // insertion will actually land in.
    growIfNeeded();

    const OrderId key = order->getOrderId();
    std::size_t index = indexFor(key, slots.size());
    while (slots[index].value != nullptr) {
        if (slots[index].key == key) {
            throw std::logic_error("Order with the same OrderId already exists in the index");
        }
        index = (index + 1) & (slots.size() - 1);
    }
    slots[index] = Slot{key, order};
    ++count;
}

/** Removes a mapping only when its pointer identity also matches. */
void OrderIndex::removeOrder(Order* order) {
    if (order == nullptr) {
        throw std::invalid_argument("Cannot remove a null order");
    }

    const std::size_t hole = findSlot(order->getOrderId());
    if (hole == static_cast<std::size_t>(-1) || slots[hole].value == nullptr) {
        throw std::logic_error("Order not found in the index");
    }
    if (slots[hole].value != order) {
        throw std::logic_error("Order pointer does not match indexed order");
    }

    slots[hole] = Slot{};
    --count;

    // Backward-shift deletion: pull each following entry in this probe run
    // back to fill the hole it left behind, as long as doing so does not
    // strand a key whose own probe run still needs to pass through here.
    std::size_t vacated = hole;
    std::size_t scan = (vacated + 1) & (slots.size() - 1);
    while (slots[scan].value != nullptr) {
        const std::size_t natural = indexFor(slots[scan].key, slots.size());
        if (!inCyclicRange(natural, vacated, scan, slots.size())) {
            slots[vacated] = slots[scan];
            slots[scan] = Slot{};
            vacated = scan;
        }
        scan = (scan + 1) & (slots.size() - 1);
    }
}

/** Performs average constant-time lookup by OrderId. */
Order* OrderIndex::findOrder(const OrderId& orderId) const {
    const std::size_t index = findSlot(orderId);
    if (index == static_cast<std::size_t>(-1)) {
        return nullptr;
    }
    return slots[index].value;
}

} // namespace lob
