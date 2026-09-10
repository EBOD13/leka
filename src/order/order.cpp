#include "lob/order/order.hpp"

#include <stdexcept>

namespace lob {

/** Constructs a new order with all submitted quantity initially remaining. */
Order::Order(OrderId orderId, Price price, Quantity originalQuantity,
             Timestamp timestamp, OrderSide orderSide, OrderType orderType,
             SequenceNumber sequenceNumber)
        : Order(orderId, price, originalQuantity, originalQuantity, timestamp,
                        orderSide, orderType, sequenceNumber) {}

/** Constructs an order with explicit submitted and remaining quantities. */
Order::Order(OrderId orderId, Price price, Quantity originalQuantity,
                         Quantity remainingQuantity, Timestamp timestamp,
                         OrderSide orderSide, OrderType orderType,
                         SequenceNumber sequenceNumber)
        : orderId(orderId), price(price), originalQuantity(originalQuantity),
            remainingQuantity(remainingQuantity), timestamp(timestamp),
            orderSide(orderSide), orderType(orderType), sequenceNumber(sequenceNumber) {}

OrderId Order::getOrderId() const { return orderId; } // Returns the order ID
Price Order::getPrice() const { return price; } // Returns the price of the order
Quantity Order::getOriginalQuantity() const { return originalQuantity; } // Returns the original quantity of the order
Quantity Order::getRemainingQuantity() const { return remainingQuantity; } // Returns the remaining quantity of the order
Timestamp Order::getTimestamp() const { return timestamp; } // Returns the timestamp of the order
OrderSide Order::getOrderSide() const { return orderSide; } // Returns the side of the order
OrderType Order::getOrderType() const { return orderType; } // Returns the type of the order
SequenceNumber Order::getSequenceNumber() const { return sequenceNumber; } // Returns the sequence number of the order

Order* Order::getNextOrder() const { return nextOrder; } // Returns the next order in the list
void Order::setNextOrder(Order* nextOrder) { this->nextOrder = nextOrder; }
Order* Order::getPreviousOrder() const { return previousOrder; } // Returns the previous order in the list
void Order::setPreviousOrder(Order* previousOrder) { this->previousOrder = previousOrder; }
PriceLevel* Order::getPriceLevel() const { return priceLevel; } // Returns the price level to which the order belongs
void Order::setPriceLevel(PriceLevel* priceLevel) { this->priceLevel = priceLevel; }

/** Reduces remaining quantity and rejects underflow. */
void Order::reduceRemainingQuantity(Quantity quantity) {
    if (quantity > remainingQuantity) {
        throw std::invalid_argument("Cannot reduce order quantity below zero");
    }

    remainingQuantity = Quantity{remainingQuantity.getQuantity() - quantity.getQuantity()};
}

/** An order is fully filled when no quantity remains. */
bool Order::isFullyFilled() const { return remainingQuantity.getQuantity() == 0; }

/** Validates identity, quantities, temporal fields, side, type, and price. */
bool Order::isValid() const {
    const bool validSide = isBuy() || isSell();
    const bool validType = isLimit() || isMarket();
    const bool validPrice = isMarket() || price.isValid();

    return orderId.isValid() && originalQuantity.isValid() &&
           remainingQuantity <= originalQuantity && timestamp.isValid() &&
           sequenceNumber.isValid() && validSide && validType && validPrice;
}

void Order::setRemainingQuantity(Quantity quantity) {
    if (!quantity.isValid()) {
        throw std::invalid_argument("Modified quantity must be valid");
    }
    if (quantity > originalQuantity) {
        throw std::invalid_argument("Modified quantity exceeds original quantity");
    }
    remainingQuantity = quantity;
}

bool Order::isBuy() const { return orderSide == OrderSide::BUY; }
bool Order::isSell() const { return orderSide == OrderSide::SELL; }
bool Order::isLimit() const { return orderType == OrderType::LIMIT; }
bool Order::isMarket() const { return orderType == OrderType::MARKET; }

} // namespace lob