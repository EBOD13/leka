// include/lob/types/order_id.hpp
#ifndef ORDER_ID_HPP
#define ORDER_ID_HPP
#include <cstdint>
#include <functional>

namespace lob {

/** @brief Type-safe identifier for an order; zero is reserved as invalid. */
class OrderId {
    private:
        std::uint64_t id;

    public:
        /** Constructs an invalid, zero-valued identifier. */
        OrderId(): id(0){}

        /** Constructs an identifier from its numeric value. */
        OrderId(std::uint64_t id): id(id){}

        /** Returns the underlying numeric identifier. */
        std::uint64_t getId() const { return id; }
        
        // Overload the equality operator to compare two OrderID objects
        bool operator==(const OrderId &other) const {
            return this->id == other.id;
        };

        /** Returns whether the identifier is nonzero. */
        bool isValid() const {
            return id != 0;
        };

        // Overload the less than operator to compare two OrderID objects
        // bool operator<(const OrderId &other) const {
        //     return this->id < other.id;
        // };

        // // Overload the greater than operator to compare two OrderID objects
        // bool operator>(const OrderId &other) const {
        //     return this->id > other.id;
        // };
        
};

} // namespace lob

namespace std {
    template <>
    struct hash<lob::OrderId> {
        std::size_t operator()(const lob::OrderId &orderId) const noexcept {
            return std::hash<std::uint64_t>{}(orderId.getId());
        }
    };
}

#endif // ORDER_ID_HPP