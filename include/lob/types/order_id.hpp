// include/lob/types/order_id.hpp
#ifndef ORDER_ID_HPP
#define ORDER_ID_HPP
#include <cstdint>
#include <functional>

namespace lob {

class OrderId {
    private:
        std::uint64_t id;

    public:
        OrderId(): id(0){} // Default constructor initilizes at 0

        OrderId(std::uint64_t id): id(id){} // Assign the id to the order

        // Getter for the order id
        std::uint64_t getId() const { return id; }
        
        // Overload the equality operator to compare two OrderID objects
        bool operator==(const OrderId &other) const {
            return this->id == other.id;
        };

        // Make sure the order id is valid (not 0)
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