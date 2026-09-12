#include "lob/book/price_ladder.hpp"

#include <bit>
#include <stdexcept>

namespace lob {

PriceLadder::PriceLadder(Price minPriceArg, Price tickSizeArg,
                          std::size_t levelCountArg, bool descendingArg)
    : minPrice(minPriceArg.getPrice()), tickSize(tickSizeArg.getPrice()),
      maxPrice(0), levelCount(levelCountArg), descending(descendingArg) {
    if (minPrice == 0) {
        throw std::invalid_argument("PriceLadder minPrice must be nonzero");
    }
    if (tickSize == 0) {
        throw std::invalid_argument("PriceLadder tickSize must be nonzero");
    }
    if (levelCount == 0) {
        throw std::invalid_argument("PriceLadder levelCount must be nonzero");
    }
    const std::uint64_t span = static_cast<std::uint64_t>(levelCount - 1);
    if (tickSize != 0 && span > (UINT64_MAX - minPrice) / tickSize) {
        throw std::invalid_argument("PriceLadder range overflows a 64-bit price");
    }
    maxPrice = minPrice + span * tickSize;

    // Every level is constructed here, once, with its permanent price. There
    // is no later insertion or erasure of a PriceLevel: only its occupancy
    // bit and its FIFO contents change for the rest of the ladder's life.
    levels.reserve(levelCount);
    for (std::size_t index = 0; index < levelCount; ++index) {
        const std::uint64_t raw = descending
            ? maxPrice - static_cast<std::uint64_t>(index) * tickSize
            : minPrice + static_cast<std::uint64_t>(index) * tickSize;
        levels.emplace_back(Price{raw});
    }
    words.assign((levelCount + 63) / 64, 0);
}

std::size_t PriceLadder::indexOf(Price price) const {
    const std::uint64_t raw = price.getPrice();
    std::uint64_t offset = 0;
    if (descending) {
        if (raw > maxPrice || raw < minPrice) {
            throw std::out_of_range("Price is outside the configured ladder range");
        }
        offset = maxPrice - raw;
    } else {
        if (raw < minPrice || raw > maxPrice) {
            throw std::out_of_range("Price is outside the configured ladder range");
        }
        offset = raw - minPrice;
    }
    if (offset % tickSize != 0) {
        throw std::invalid_argument("Price does not fall on a configured tick boundary");
    }
    return static_cast<std::size_t>(offset / tickSize);
}

PriceLevel& PriceLadder::levelAt(Price price) {
    return levels[indexOf(price)];
}

/**
 * @details Setting bits and lowering a cached minimum are both O(1); nothing
 * here ever searches.
 */
void PriceLadder::markOccupied(Price price) {
    const std::size_t index = indexOf(price);
    words[index / 64] |= (std::uint64_t{1} << (index % 64));
    if (occupied == 0 || index < bestIndex) {
        bestIndex = index;
    }
    ++occupied;
}

/**
 * @details Clearing a bit is O(1). Re-deriving the best index is only needed
 * when the level that just emptied WAS the best index; every other call is
 * O(1) as well. That one case scans forward from the vacated index for the
 * next set bit, which is a handful of instructions per 64-bit word rather
 * than a per-level check, and in practice terminates almost immediately
 * because resting liquidity clusters near the touch.
 */
void PriceLadder::markEmpty(Price price) {
    const std::size_t index = indexOf(price);
    words[index / 64] &= ~(std::uint64_t{1} << (index % 64));
    --occupied;
    if (index == bestIndex) {
        bestIndex = occupied == 0 ? npos : nextSetBit(index + 1);
    }
}

std::size_t PriceLadder::nextSetBit(std::size_t from) const {
    if (from >= levelCount) {
        return npos;
    }
    const std::size_t startWord = from / 64;
    const unsigned startBit = static_cast<unsigned>(from % 64);

    const std::uint64_t firstWord = words[startWord] >> startBit;
    if (firstWord != 0) {
        return from + static_cast<std::size_t>(std::countr_zero(firstWord));
    }
    for (std::size_t word = startWord + 1; word < words.size(); ++word) {
        if (words[word] != 0) {
            const std::size_t index =
                word * 64 + static_cast<std::size_t>(std::countr_zero(words[word]));
            return index < levelCount ? index : npos;
        }
    }
    return npos;
}

PriceLevel* PriceLadder::best() {
    return bestIndex == npos ? nullptr : &levels[bestIndex];
}

const PriceLevel* PriceLadder::best() const {
    return bestIndex == npos ? nullptr : &levels[bestIndex];
}

} // namespace lob
