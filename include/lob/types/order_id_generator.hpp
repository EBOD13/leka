// include/lob/types/order_id_generator.hpp
#ifndef ORDER_ID_GENERATOR_HPP
#define ORDER_ID_GENERATOR_HPP

#include "lob/types/order_id.hpp"

namespace lob {

/** @brief Generates monotonically increasing order identifiers. */
class OrderIdGenerator {
    private:
        std::uint64_t currentId{0}; // Static member variable to keep track of the current order id

    public:        
    /** Returns the next valid order identifier or throws on exhaustion. */
    OrderId generate();
};

} // namespace lob

#endif