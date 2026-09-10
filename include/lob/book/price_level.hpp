#ifndef PRICE_LEVEL_HPP
#define PRICE_LEVEL_HPP
#include "lob/types/price.hpp"
#include "lob/order/order.hpp"
#include "lob/types/quantity.hpp"
#include <cstddef>

namespace lob {
/**
 * @brief Maintains FIFO resting orders at one price.
 *
 * Orders are linked intrusively, so appending and removing a known order are
 * constant-time operations. The level also tracks order count and aggregate
 * remaining quantity.
 */
class PriceLevel {
    private:
        Price price;
        Order* headOrder{nullptr};
        Order* tailOrder{nullptr};

        std::size_t count{0}; // Counts of orders we currently have - init at 0

        Quantity totalQuantity{0}; // Total quantity of orders at this price level - init at 0

    public:
        /** Creates an empty level for a single price. */
        explicit PriceLevel(Price price) : price(price) {}

        /** Returns the level price. */
        Price getPrice() const { return price; }
        /** Returns the first resting order, or nullptr when empty. */
        Order* getHeadOrder() const { return headOrder; }
        /** Returns the last resting order, or nullptr when empty. */
        Order* getTailOrder() const { return tailOrder; }
        /** Returns the number of resting orders. */
        std::size_t getOrderCount() const { return count; }
        /** Returns the aggregate remaining quantity. */
        Quantity getTotalQuantity() const { return totalQuantity; }

        /** Appends an order to the FIFO and updates aggregate state. */
        void addOrder(Order* order);
        /** Unlinks an order and updates aggregate state. */
        void removeOrder(Order* order);

        /** Decreases aggregate quantity after a partial execution. */
        void reduceTotalQuantity(Quantity quantity);

        /** Returns true when no orders are resting at this price. */
        bool isEmpty() const;
};
} // namespace lob
#endif // PRICE_LEVEL_HPP