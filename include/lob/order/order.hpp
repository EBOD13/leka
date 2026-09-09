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
		Order(OrderId orderId, Price price, Quantity originalQuantity,
			  Timestamp timestamp, OrderSide orderSide, OrderType orderType,
			  SequenceNumber sequenceNumber);

		OrderId getOrderId() const;
		Price getPrice() const;
		Quantity getOriginalQuantity() const;
		Quantity getRemainingQuantity() const;
		Timestamp getTimestamp() const;
		OrderSide getOrderSide() const;
		OrderType getOrderType() const;
		SequenceNumber getSequenceNumber() const;

		Order* getNextOrder() const;
		void setNextOrder(Order* nextOrder);
		Order* getPreviousOrder() const;
		void setPreviousOrder(Order* previousOrder);
		PriceLevel* getPriceLevel() const;
		void setPriceLevel(PriceLevel* priceLevel);

		void reduceRemainingQuantity(Quantity quantity);
		bool isFullyFilled() const;
		bool isValid() const;
		bool isBuy() const;
		bool isSell() const;
		bool isLimit() const;
		bool isMarket() const;
};

} // namespace lob

#endif // ORDER_HPP
