#include "lob/order/order_event.hpp"

#include <stdexcept>
#include <utility>

namespace lob {

/** @file Implements type-safe NEW, CANCEL, and REDUCE event payload access. */

/** Constructs a NEW event from its payload. */
OrderEvent::OrderEvent(NewOrder order) : payload(std::move(order)) {}

/** Constructs a CANCEL event from its payload. */
OrderEvent::OrderEvent(CancelOrder order) : payload(order) {}

/** Constructs a REDUCE event from its payload. */
OrderEvent::OrderEvent(ReduceOrder order) : payload(order) {}

/** Returns the variant index corresponding to the event operation. */
OrderEventType OrderEvent::getEventType() const {
    return static_cast<OrderEventType>(payload.index());
}

/**
 * @details The accessors test the active alternative once through get_if
 * rather than pairing holds_alternative with get, which would discriminate
 * the variant twice on every dispatched event.
 */
const NewOrder& OrderEvent::getNewOrder() const {
    const NewOrder* order = std::get_if<NewOrder>(&payload);
    if (order == nullptr) {
        throw std::logic_error("Order event is not NEW");
    }
    return *order;
}

/** Returns the CANCEL payload after checking the active event type. */
const CancelOrder& OrderEvent::getCancelOrder() const {
    const CancelOrder* order = std::get_if<CancelOrder>(&payload);
    if (order == nullptr) {
        throw std::logic_error("Order event is not CANCEL");
    }
    return *order;
}

/** Returns the REDUCE payload after checking the active event type. */
const ReduceOrder& OrderEvent::getReduceOrder() const {
    const ReduceOrder* order = std::get_if<ReduceOrder>(&payload);
    if (order == nullptr) {
        throw std::logic_error("Order event is not REDUCE");
    }
    return *order;
}

} // namespace lob
