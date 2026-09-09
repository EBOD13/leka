// src/index/order_index.cpp

#include "lob/index/order_index.hpp"
#include <stdexcept>

namespace lob {

    void OrderIndex::addOrder(Order* order) {
        if (order == nullptr) {
            throw std::invalid_argument("Cannot add a null order");
        }
        if (!order->isValid()) {
            throw std::invalid_argument("Cannot add an invalid order");
        }
        auto result = orders.emplace(order->getOrderId(), order);
        if (!result.second) {
            throw std::logic_error("Order with the same OrderId already exists in the index");
        }
    }

    void OrderIndex::removeOrder(Order* order){
        if(order == nullptr) {
            throw std::invalid_argument("Cannot remove a null order");
        }
        auto it = orders.find(order->getOrderId());
        if(it == orders.end()) {
            throw std::logic_error("Order not found in the index");
        }
        if (it->second != order) {
            throw std::logic_error("Order pointer does not match indexed order");
        }
        orders.erase(it);
    }

    Order* OrderIndex::findOrder(const OrderId& orderId) const {
        auto it = orders.find(orderId);
        if(it == orders.end()) {
            return nullptr;
        }
        return it->second;
    }

}