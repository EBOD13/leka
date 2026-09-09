#ifndef QUANTITY_HPP
#define QUANTITY_HPP
#include <cstdint>
#include <compare>

namespace lob {
class Quantity {
    private:
        std::uint64_t quantity;

    public:
        Quantity(): quantity(0){} // Default constructor init at 0

        Quantity(std::uint64_t quantity): quantity(quantity){} // Assign the quantity to the order

        // Getter for the quantity
        std::uint64_t getQuantity() const { return quantity;};

        // Make sure the quantity is valid (not 0)
        bool isValid() const {
            return quantity != 0;
        };

        auto operator<=>(const Quantity &other) const = default;
        Quantity operator+(const Quantity &other) const {
            return Quantity{this->quantity + other.quantity};
        };
        
};
} // namespace lob
#endif