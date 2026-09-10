// include/lob/order/order_type.hpp
#ifndef ORDER_TYPE_HPP
#define ORDER_TYPE_HPP

namespace lob {
/** @brief Execution and resting behavior requested for an order. */
enum class OrderType {
    /** Executes immediately when crossing, then may rest if supported. */
    LIMIT, 
    /** Executes against available liquidity and never rests in OrderBook. */
    MARKET
};
} // namespace lob

#endif