#ifndef ORDER_EVENT_HPP
#define ORDER_EVENT_HPP

#include "lob/order/order_side.hpp"
#include "lob/order/order_type.hpp"
#include "lob/types/order_id.hpp"
#include "lob/types/price.hpp"
#include "lob/types/quantity.hpp"
#include "lob/types/timestamp.hpp"

#include <variant>

/** @file Defines event-first order command payloads and dispatch types. */

namespace lob {

/**
 * @brief Operation selected before an event payload is interpreted.
 *
 * These are the three primitives a price-time-priority venue actually
 * exposes. There is deliberately no in-place modify: a reprice or a size
 * increase always forfeits time priority, so it is expressed as CANCEL
 * followed by NEW, which also routes it through the matcher. REDUCE is the
 * only change that keeps a resting order's queue position.
 */
enum class OrderEventType {
    NEW,
    CANCEL,
    REDUCE
};

/** @brief Payload for accepting a new order. */
struct NewOrder {
    /** Unique identifier for the new order. */
    OrderId orderId;
    /** Limit price, or an invalid price for a market order. */
    Price price;
    /** Quantity submitted by the caller. */
    Quantity quantity;
    /** Timestamp assigned to the order. */
    Timestamp timestamp;
    /** Buy or sell direction. */
    OrderSide orderSide;
    /** Limit or market execution behavior. */
    OrderType orderType;
};

/** @brief Payload for removing an existing resting order. */
struct CancelOrder {
    /** Identifier of the order to remove. */
    OrderId orderId;
};

/**
 * @brief Payload for shrinking a resting order without losing priority.
 *
 * The price cannot change, so the order never moves between levels and keeps
 * its FIFO position. A reduction to zero is a cancellation and must be sent
 * as CancelOrder instead.
 */
struct ReduceOrder {
    /** Identifier of the order to shrink. */
    OrderId orderId;
    /** Replacement remaining quantity; nonzero and not above the current one. */
    Quantity newQuantity;
};

/**
 * @brief Type-safe event-first command for the matching engine.
 *
 * The active payload determines which fields are relevant. Accessing a
 * payload for a different event type throws std::logic_error.
 */
class OrderEvent {
    public:
        /** Creates a NEW event. */
        explicit OrderEvent(NewOrder order);
        /** Creates a CANCEL event. */
        explicit OrderEvent(CancelOrder order);
        /** Creates a REDUCE event. */
        explicit OrderEvent(ReduceOrder order);

        /** Returns the operation represented by the active payload. */
        OrderEventType getEventType() const;
        /** Returns the NEW payload or throws when this is not a NEW event. */
        const NewOrder& getNewOrder() const;
        /** Returns the CANCEL payload or throws when this is not a CANCEL event. */
        const CancelOrder& getCancelOrder() const;
        /** Returns the REDUCE payload or throws when this is not a REDUCE event. */
        const ReduceOrder& getReduceOrder() const;

    private:
        std::variant<NewOrder, CancelOrder, ReduceOrder> payload;
};

} // namespace lob

#endif // ORDER_EVENT_HPP