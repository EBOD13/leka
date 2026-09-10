#ifndef QUANTITY_HPP
#define QUANTITY_HPP
#include <cstdint>
#include <compare>

namespace lob {
/** @brief Type-safe unsigned order quantity; zero represents a filled state. */
class Quantity {
    private:
        std::uint64_t quantity;

    public:
        /** Constructs a zero quantity. */
        Quantity(): quantity(0){}

        /** Constructs a quantity from its numeric value. */
        Quantity(std::uint64_t quantity): quantity(quantity){}

        /** Returns the underlying numeric quantity. */
        std::uint64_t getQuantity() const { return quantity;};

        /** Returns whether the quantity is valid for a submitted order. */
        bool isValid() const {
            return quantity != 0;
        };

        /** Compares quantities by numeric value. */
        auto operator<=>(const Quantity &other) const = default;
        /** Adds quantities while preserving the Quantity type. */
        Quantity operator+(const Quantity &other) const {
            return Quantity{this->quantity + other.quantity};
        };
        
};
} // namespace lob
#endif