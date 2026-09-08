// src/order/order_id_generator.cpp
#include "lob/types/order_id_generator.hpp"

#include <limits>
#include <stdexcept>

namespace lob {

OrderId OrderIdGenerator::generate() {
    if (currentId == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Order ID space exhausted"); // throw an exception if the maximum value is reached
    }

    return OrderId{++currentId};
}

} // namespace lob