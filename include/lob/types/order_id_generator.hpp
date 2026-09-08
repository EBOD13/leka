#ifndef ORDER_ID_GENERATOR_HPP
#define ORDER_ID_GENERATOR_HPP

#include "lob/types/order_id.hpp"

namespace lob {

class OrderIdGenerator {
    private:
        std::uint64_t currentId{0}; // Static member variable to keep track of the current order id

    public:        
    OrderId generate();
};

} // namespace lob

#endif