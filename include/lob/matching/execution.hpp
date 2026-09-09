// include/lob/matching/execution.hpp
#ifndef EXECUTION_HPP
#define EXECUTION_HPP

#include "lob/types/price.hpp"
#include "lob/types/quantity.hpp"
#include "lob/types/order_id.hpp"

namespace lob {
    class Execution {
        private:
            OrderId incomingOrderId; // The ID of the incoming order that triggered the execution
            OrderId restingOrderId; // The ID of the resting order that was matched with the incoming order
            Price executionPrice; // The price at which the execution occurred
            Quantity executionQuantity; // The quantity of the order that was executed
        
        public:
            Execution(OrderId incomingOrderId, OrderId restingOrderId, Price executionPrice, Quantity executionQuantity);

            OrderId getIncomingOrderId() const;
            OrderId getRestingOrderId() const;
            Price getExecutionPrice() const;
            Quantity getExecutionQuantity() const;
            bool isValid() const; 
    };
} // namespace lob
#endif // EXECUTION_HPP