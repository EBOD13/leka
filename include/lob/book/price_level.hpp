#ifndef PRICE_LEVEL_HPP
#define PRICE_LEVEL_HPP
#include "lob/types/price.hpp"
#include "lob/order/order.hpp"
#include "lob/types/quantity.hpp"
#include <cstddef>

namespace lob {
class PriceLevel {
    private:
        Price price;
        Order* headOrder{nullptr};
        Order* tailOrder{nullptr};

        std::size_t count{0}; // Counts of orders we currently have - init at 0

        Quantity totalQuantity{0}; // Total quantity of orders at this price level - init at 0

    public:
        explicit PriceLevel(Price price) : price(price) {}

        Price getPrice() const { return price; }
        Order* getHeadOrder() const { return headOrder; }
        Order* getTailOrder() const { return tailOrder; }
        std::size_t getOrderCount() const { return count; }
        Quantity getTotalQuantity() const { return totalQuantity; }

        void addOrder(Order* order);
        void removeOrder(Order* order);

        void reduceTotalQuantity(Quantity quantity);

        bool isEmpty() const;
};
} // namespace lob
#endif // PRICE_LEVEL_HPP