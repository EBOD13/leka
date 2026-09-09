//include/lob/index/order_index.hpp

#ifndef ORDER_INDEX_HPP
#define ORDER_INDEX_HPP
#include "lob/types/order_id.hpp"
#include "lob/order/order.hpp"
#include <unordered_map>

namespace lob {
class OrderIndex {
    private:
        std::unordered_map<OrderId, Order*> orders; // Map to store orders by their OrderId
    
        public:
        // Add an order to the index
        void addOrder(Order* order);

        // Remove an order from the index
        void removeOrder(Order* order);

        // Find an order by its OrderId
        Order* findOrder(const OrderId& orderId) const;
};
} // namespace lob
#endif // ORDER_INDEX_HPP