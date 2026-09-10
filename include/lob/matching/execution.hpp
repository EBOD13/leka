// include/lob/matching/execution.hpp
#ifndef EXECUTION_HPP
#define EXECUTION_HPP

#include "lob/types/price.hpp"
#include "lob/types/quantity.hpp"
#include "lob/types/order_id.hpp"

namespace lob {
/** @brief Describes one match between an incoming and resting order. */
    class Execution {
        private:
            OrderId incomingOrderId;
            OrderId restingOrderId;
            Price executionPrice;
            Quantity executionQuantity;
        
        public:
            /** Constructs an execution at the resting order's price. */
            Execution(OrderId incomingOrderId, OrderId restingOrderId, Price executionPrice, Quantity executionQuantity);

            /** Returns the incoming order ID. */
            OrderId getIncomingOrderId() const;
            /** Returns the resting order ID. */
            OrderId getRestingOrderId() const;
            /** Returns the execution price. */
            Price getExecutionPrice() const;
            /** Returns the executed quantity. */
            Quantity getExecutionQuantity() const;
            /** Returns whether all execution fields are valid. */
            bool isValid() const; 
    };
} // namespace lob
#endif // EXECUTION_HPP