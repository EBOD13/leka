// src/book/order_pool.cpp

#include "lob/book/order_pool.hpp"

#include <cerrno>
#include <new>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <unistd.h>

namespace lob {

OrderPool::OrderPool() = default;

struct OrderPool::Page {
    void* storage; // Pointer to the allocated memory for the page
    std::array<std::uint16_t, SlotsPerPage> nextFree{}; // Array to keep track of the next free slot in the page
    std::array<bool, SlotsPerPage> occupied{}; // Array to keep track of which slots are occupied
    std::uint16_t firstFree{0}; // Index of the first free slot in the page
    std::size_t freeCount{SlotsPerPage}; // Count of free slots in the page
    Page* previousFreePage{nullptr}; // Pointer to the previous page in the list of pages with free slots
    Page* nextFreePage{nullptr}; // Pointer to the next page in the list of pages with free slots
    bool onFreePageList{false}; // Flag to indicate whether the page is on the list of pages with free slots

    /** Allocates one aligned page and initializes its external free list. */
    Page() {
        const long systemPageSize = sysconf(_SC_PAGESIZE);
        if (systemPageSize <= 0 || PageSize % static_cast<std::size_t>(systemPageSize) != 0) {
            throw std::runtime_error("OrderPool page size is incompatible with the system page size");
        }

        const std::size_t allocationSize = PageSize * 2;
        void* allocation = mmap(nullptr, allocationSize, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (allocation == MAP_FAILED) {
            throw std::system_error(errno, std::generic_category(), "mmap failed");
        }

        const auto address = reinterpret_cast<std::uintptr_t>(allocation);
        const auto alignedAddress = (address + PageSize - 1) & ~(PageSize - 1);
        const auto prefixSize = alignedAddress - address;
        const auto suffixSize = allocationSize - prefixSize - PageSize;
        if (prefixSize != 0) {
            munmap(reinterpret_cast<void*>(address), prefixSize);
        }
        if (suffixSize != 0) {
            munmap(reinterpret_cast<void*>(alignedAddress + PageSize), suffixSize);
        }

        storage = reinterpret_cast<void*>(alignedAddress);
        for (std::size_t index = 0; index + 1 < SlotsPerPage; ++index) {
            nextFree[index] = static_cast<std::uint16_t>(index + 1);
        }
        nextFree[SlotsPerPage - 1] = SlotsPerPage;
    }

    /** Returns the page mapping to the operating system. */
    ~Page() {
        munmap(storage, PageSize);
    }

    Order* rawSlot(std::size_t index) {
        return reinterpret_cast<Order*>(
            static_cast<std::byte*>(storage) + index * sizeof(Order));
    }

    Order* slot(std::size_t index) {
        return std::launder(reinterpret_cast<Order*>(
            static_cast<std::byte*>(storage) + index * sizeof(Order)));
    }
};

OrderPool::~OrderPool() {
    for (const auto& page : pages) {
        for (std::size_t index = 0; index < SlotsPerPage; ++index) {
            if (page->occupied[index]) {
                std::destroy_at(page->slot(index));
            }
        }
    }
}

/** Creates and registers a page without moving existing pages. */
OrderPool::Page* OrderPool::createPage() {
    auto page = std::make_unique<Page>();
    Page* pagePointer = page.get();
    pages.push_back(std::move(page));
    const auto pageBase = reinterpret_cast<std::uintptr_t>(pagePointer->storage);
    try {
        const auto result = pageIndex.emplace(pageBase, pagePointer);
        if (!result.second) {
            pages.pop_back();
            throw std::logic_error("Duplicate OrderPool page address");
        }
    } catch (...) {
        if (pageIndex.find(pageBase) == pageIndex.end()) {
            pages.pop_back();
        }
        throw;
    }
    addToFreePageList(pagePointer);
    return pagePointer;
}

void OrderPool::addToFreePageList(Page* page) {
    if (page->onFreePageList) {
        return;
    }
    page->previousFreePage = nullptr;
    page->nextFreePage = firstPageWithFreeSlot;
    if (firstPageWithFreeSlot != nullptr) {
        firstPageWithFreeSlot->previousFreePage = page;
    }
    firstPageWithFreeSlot = page;
    page->onFreePageList = true;
}

void OrderPool::removeFromFreePageList(Page* page) {
    if (!page->onFreePageList) {
        return;
    }
    if (page->previousFreePage != nullptr) {
        page->previousFreePage->nextFreePage = page->nextFreePage;
    } else {
        firstPageWithFreeSlot = page->nextFreePage;
    }
    if (page->nextFreePage != nullptr) {
        page->nextFreePage->previousFreePage = page->previousFreePage;
    }
    page->previousFreePage = nullptr;
    page->nextFreePage = nullptr;
    page->onFreePageList = false;
}

Order* OrderPool::allocate(OrderId orderId, Price price, Quantity originalQuantity,
                           Timestamp timestamp, OrderSide orderSide,
                           OrderType orderType, SequenceNumber sequenceNumber) {
	return allocate(orderId, price, originalQuantity, originalQuantity, timestamp,
					orderSide, orderType, sequenceNumber);
}

Order* OrderPool::allocate(OrderId orderId, Price price, Quantity originalQuantity,
                           Quantity remainingQuantity, Timestamp timestamp,
                           OrderSide orderSide, OrderType orderType,
                           SequenceNumber sequenceNumber) {
    Page* page = firstPageWithFreeSlot;
    if (page == nullptr) {
        page = createPage();
    }

    const std::size_t index = page->firstFree;
    Order* order = std::construct_at(page->rawSlot(index), orderId, price,
                                      originalQuantity, remainingQuantity,
                                      timestamp, orderSide, orderType,
                                      sequenceNumber);

    page->firstFree = page->nextFree[index];
    --page->freeCount;
    page->occupied[index] = true;
    if (page->freeCount == 0) {
        removeFromFreePageList(page);
    }

    ++liveOrderCount;
    return order;
}

/** Releases a live slot after validating pool ownership and allocation state. */
void OrderPool::release(Order* order) {
    if (order == nullptr) {
        throw std::invalid_argument("Cannot release a null order");
    }

    const auto address = reinterpret_cast<std::uintptr_t>(order);
    const auto pageBase = address & ~(PageSize - 1);
    const auto pageIterator = pageIndex.find(pageBase);
    if (pageIterator == pageIndex.end()) {
        throw std::invalid_argument("Order does not belong to this pool");
    }

    Page* page = pageIterator->second;
    const auto offset = address - pageBase;
    if (offset % sizeof(Order) != 0) {
        throw std::invalid_argument("Pointer is not an Order slot");
    }
    const std::size_t index = offset / sizeof(Order);
    if (index >= SlotsPerPage || !page->occupied[index]) {
        throw std::invalid_argument("Order slot is not allocated");
    }

    std::destroy_at(order);
    page->occupied[index] = false;
    page->nextFree[index] = page->firstFree;
    page->firstFree = static_cast<std::uint16_t>(index);
    if (page->freeCount == 0) {
        addToFreePageList(page);
    }
    ++page->freeCount;
    --liveOrderCount;
}

} // namespace lob