#include "lob/order/order_event.hpp"

#include <stdexcept>
#include <utility>

namespace lob {

/** @file Implements type-safe NEW, CANCEL, and MODIFY event payload access. */

/** Constructs a NEW event from its payload. */
OrderEvent::OrderEvent(NewOrder order) : payload(std::move(order)) {}

/** Constructs a CANCEL event from its payload. */
OrderEvent::OrderEvent(CancelOrder order) : payload(order) {}

/** Constructs a MODIFY event from its payload. */
OrderEvent::OrderEvent(ModifyOrder order) : payload(order) {}

/** Returns the variant index corresponding to the event operation. */
OrderEventType OrderEvent::getEventType() const {
    return static_cast<OrderEventType>(payload.index());
}

/** Returns the NEW payload after checking the active event type. */
const NewOrder& OrderEvent::getNewOrder() const {
    if (!std::holds_alternative<NewOrder>(payload)) {
        throw std::logic_error("Order event is not NEW");
    }
    return std::get<NewOrder>(payload);
}

/** Returns the CANCEL payload after checking the active event type. */
const CancelOrder& OrderEvent::getCancelOrder() const {
    if (!std::holds_alternative<CancelOrder>(payload)) {
        throw std::logic_error("Order event is not CANCEL");
    }
    return std::get<CancelOrder>(payload);
}

/** Returns the MODIFY payload after checking the active event type. */
const ModifyOrder& OrderEvent::getModifyOrder() const {
    if (!std::holds_alternative<ModifyOrder>(payload)) {
        throw std::logic_error("Order event is not MODIFY");
    }
    return std::get<ModifyOrder>(payload);
}

} // namespace lob