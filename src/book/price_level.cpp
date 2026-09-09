#include "lob/book/price_level.hpp"

#include <limits>
#include <stdexcept>

namespace lob {

// Add an order to the price level
void PriceLevel::addOrder(Order* order) {
	if (order == nullptr) {
		throw std::invalid_argument("Cannot add a null order");
	}
	if ((order->getPrice() <=> price) != 0) {
		throw std::invalid_argument("Order price does not match price level");
	}
	if (order->getPriceLevel() != nullptr) {
		throw std::invalid_argument("Order already belongs to a price level");
	}
	if (order->isFullyFilled()) {
		throw std::invalid_argument("Cannot add a fully filled order");
	}
	if (order->getRemainingQuantity().getQuantity() >
		std::numeric_limits<std::uint64_t>::max() - totalQuantity.getQuantity()) {
		throw std::overflow_error("Price level quantity overflow");
	}

	order->setPreviousOrder(tailOrder); // Add the order to the end of the list
	order->setNextOrder(nullptr); // Set the next order to nullptr since it's the last order in the list
	if (tailOrder != nullptr) {
		tailOrder->setNextOrder(order);
	} else {
		headOrder = order;
	}
	tailOrder = order;
	++count;
	totalQuantity = totalQuantity + order->getRemainingQuantity();
	order->setPriceLevel(this);
}

// Remove an order from the price level
void PriceLevel::removeOrder(Order* order) {
	if (order == nullptr || order->getPriceLevel() != this) {
		throw std::invalid_argument("Order does not belong to this price level");
	}

	Order* previousOrder = order->getPreviousOrder();
	Order* nextOrder = order->getNextOrder();
    // Update the previous and next orders to bypass the removed order
	if (previousOrder != nullptr) {
		previousOrder->setNextOrder(nextOrder);
	} else {
		headOrder = nextOrder;
	}

	if (nextOrder != nullptr) {
		nextOrder->setPreviousOrder(previousOrder);
	} else {
		tailOrder = previousOrder;
	}

	totalQuantity = Quantity{totalQuantity.getQuantity() -
							 order->getRemainingQuantity().getQuantity()};
	--count;
	order->setPreviousOrder(nullptr);
	order->setNextOrder(nullptr);
	order->setPriceLevel(nullptr);
}

// Reduce the total quantity of the price level by the specified amount
void PriceLevel::reduceTotalQuantity(Quantity quantity) {
	if (quantity > totalQuantity) {
		throw std::invalid_argument("Cannot reduce price level quantity below zero");
	}

	totalQuantity = Quantity{totalQuantity.getQuantity() - quantity.getQuantity()};
}

bool PriceLevel::isEmpty() const {
	return count == 0;
}

} // namespace lob
