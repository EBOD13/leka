// include/lob/order/order_side.hpp
#ifndef ORDER_SIDE_HPP
#define ORDER_SIDE_HPP

namespace lob {
/** @brief Side of the market on which an order rests or executes. */
enum class OrderSide {
    /** Buy-side order. */
    BUY,
    /** Sell-side order. */
    SELL
};
} // namespace lob

#endif