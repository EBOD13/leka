//include/lob/index/order_index.hpp

#ifndef ORDER_INDEX_HPP
#define ORDER_INDEX_HPP
#include "lob/types/order_id.hpp"
#include "lob/order/order.hpp"
#include <unordered_map>

namespace lob {
/** @brief Provides average constant-time lookup from OrderId to Order*. */
class OrderIndex {
    private:
        std::unordered_map<OrderId, Order*> orders; // Map to store orders by their OrderId
    
        public:
        /** Adds an order and rejects duplicate IDs. */
        void addOrder(Order* order);

        /** Removes an order after verifying pointer identity. */
        void removeOrder(Order* order);

        /** Finds an order by ID, or returns nullptr when absent. */
        Order* findOrder(const OrderId& orderId) const;
};
} // namespace lob
#endif // ORDER_INDEX_HPP