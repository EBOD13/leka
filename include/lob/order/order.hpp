#ifndef ORDER_HPP
#define ORDER_HPP

#include "lob/order/order_side.hpp"
#include "lob/order/order_type.hpp"
#include "lob/types/order_id.hpp"
#include "lob/types/price.hpp"
#include "lob/types/quantity.hpp"
#include "lob/types/sequence_number.hpp"
#include "lob/types/timestamp.hpp"

namespace lob {

class PriceLevel;

/** @brief Represents a resting or incoming order and its lifecycle state. */
class Order {
	private:
		OrderId orderId;
		Price price;
		Quantity originalQuantity;
		Quantity remainingQuantity;
		Timestamp timestamp;
		OrderSide orderSide;
		OrderType orderType;
		SequenceNumber sequenceNumber;
		Order* nextOrder{nullptr};
		Order* previousOrder{nullptr};
		PriceLevel* priceLevel{nullptr};

	public:
		/** Constructs an order with remaining quantity equal to the original quantity. */
		Order(OrderId orderId, Price price, Quantity originalQuantity,
			  Timestamp timestamp, OrderSide orderSide, OrderType orderType,
			  SequenceNumber sequenceNumber);
		/** Constructs an order with explicit original and remaining quantities. */
		Order(OrderId orderId, Price price, Quantity originalQuantity,
			  Quantity remainingQuantity, Timestamp timestamp, OrderSide orderSide,
			  OrderType orderType, SequenceNumber sequenceNumber);

		/** Returns the unique order identifier. */
		OrderId getOrderId() const;
		/** Returns the limit or reference price. */
		Price getPrice() const;
		/** Returns the quantity submitted when the order was created. */
		Quantity getOriginalQuantity() const;
		/** Returns the quantity that has not yet executed. */
		Quantity getRemainingQuantity() const;
		/** Returns the order timestamp. */
		Timestamp getTimestamp() const;
		/** Returns whether this is a buy or sell order. */
		OrderSide getOrderSide() const;
		/** Returns the order type. */
		OrderType getOrderType() const;
		/** Returns the deterministic processing sequence. */
		SequenceNumber getSequenceNumber() const;

		Order* getNextOrder() const;
		void setNextOrder(Order* nextOrder);
		Order* getPreviousOrder() const;
		void setPreviousOrder(Order* previousOrder);
		PriceLevel* getPriceLevel() const;
		void setPriceLevel(PriceLevel* priceLevel);

		/** Decreases remaining quantity; throws if the reduction would underflow. */
		void reduceRemainingQuantity(Quantity quantity);
		/** Returns true when remaining quantity is zero. */
		bool isFullyFilled() const;
		/** Returns whether the order state satisfies its structural invariants. */
		bool isValid() const;
		/** Updates quantity while the order remains at its current price level. */
		void setRemainingQuantity(Quantity quantity);
		/** Returns true for a buy order. */
		bool isBuy() const;
		/** Returns true for a sell order. */
		bool isSell() const;
		/** Returns true for a limit order. */
		bool isLimit() const;
		/** Returns true for a market order. */
		bool isMarket() const;
};

} // namespace lob

#endif // ORDER_HPP
