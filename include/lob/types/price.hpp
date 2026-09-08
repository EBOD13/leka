#ifndef PRICE_HPP
#define PRICE_HPP
#include <cstdint>
#include <compare>

namespace lob {
class Price {
    private:
        std::uint64_t price;

    public:
        Price(): price(0){} // Default constructor init at 0

        Price(std::uint64_t price): price(price){} // Assign the price to the order

        // Getter for the price
        std::uint64_t getPrice() const { return price;};

        // Overload operators to compare two Price objects
        auto operator<=>(const Price &other) const {
            return this->price <=> other.price;
        };
        // Make sure the price is valid (not 0)
        bool isValid() const {
            return price != 0;
        };
};
} // namespace lob

#endif