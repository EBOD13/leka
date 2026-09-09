#ifndef SEQUENCE_NUMBER_GENERATOR_HPP
#define SEQUENCE_NUMBER_GENERATOR_HPP

#include "lob/types/sequence_number.hpp"

namespace lob {

class SequenceNumberGenerator {
    private:
        static std::uint64_t currentSequence;

    public:
        static SequenceNumber generate();
};

} // namespace lob

#endif // SEQUENCE_NUMBER_GENERATOR_HPP