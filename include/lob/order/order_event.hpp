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

/** @brief Operation selected before an event payload is interpreted. */
enum class OrderEventType {
    NEW,
    CANCEL,
    MODIFY
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

/** @brief Payload for changing an existing resting limit order. */
struct ModifyOrder {
    /** Identifier of the order to modify. */
    OrderId orderId;
    /** Replacement limit price. */
    Price newPrice;
    /** Replacement remaining quantity. */
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
        /** Creates a MODIFY event. */
        explicit OrderEvent(ModifyOrder order);

        /** Returns the operation represented by the active payload. */
        OrderEventType getEventType() const;
        /** Returns the NEW payload or throws when this is not a NEW event. */
        const NewOrder& getNewOrder() const;
        /** Returns the CANCEL payload or throws when this is not a CANCEL event. */
        const CancelOrder& getCancelOrder() const;
        /** Returns the MODIFY payload or throws when this is not a MODIFY event. */
        const ModifyOrder& getModifyOrder() const;

    private:
        std::variant<NewOrder, CancelOrder, ModifyOrder> payload;
};

} // namespace lob

#endif // ORDER_EVENT_HPP