// src/matching/execution.cpp
#include "lob/matching/execution.hpp"

namespace lob {
    Execution::Execution(OrderId incomingOrderId, OrderId restingOrderId, Price executionPrice, Quantity executionQuantity)
        : incomingOrderId(incomingOrderId), restingOrderId(restingOrderId), executionPrice(executionPrice), executionQuantity(executionQuantity) {}

    OrderId Execution::getIncomingOrderId() const {
        return incomingOrderId;
    }

    OrderId Execution::getRestingOrderId() const {
        return restingOrderId;
    }

    Price Execution::getExecutionPrice() const {
        return executionPrice;
    }

    Quantity Execution::getExecutionQuantity() const {
        return executionQuantity;
    }

    bool Execution::isValid() const {
        return incomingOrderId.isValid() && restingOrderId.isValid() && executionPrice.isValid() && executionQuantity.isValid();
    }
} // namespace lob
