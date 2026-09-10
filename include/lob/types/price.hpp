// include/lob/types/price.hpp
#ifndef PRICE_HPP
#define PRICE_HPP
#include <cstdint>
#include <compare>

namespace lob {
/** @brief Type-safe nonzero price value used for price ordering. */
class Price {
    private:
        std::uint64_t price;

    public:
        /** Constructs an invalid, zero-valued price. */
        Price(): price(0){}

        /** Constructs a price from its numeric value. */
        Price(std::uint64_t price): price(price){}

        /** Returns the underlying numeric price. */
        std::uint64_t getPrice() const { return price;};

        /** Compares prices chronologically by numeric value. */
        auto operator<=>(const Price &other) const {
            return this->price <=> other.price;
        };
        /** Returns whether the price is nonzero. */
        bool isValid() const {
            return price != 0;
        };
};
} // namespace lob

#endif