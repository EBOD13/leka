#ifndef SEQUENCE_NUMBER_GENERATOR_HPP
#define SEQUENCE_NUMBER_GENERATOR_HPP

#include "lob/types/sequence_number.hpp"

namespace lob {

/**
 * @brief Generates process-wide monotonic sequence numbers.
 *
 * Sequence numbers identify accepted engine processing order and are assigned
 * only after an incoming order passes validation and duplicate checks.
 */
class SequenceNumberGenerator {
    private:
        static std::uint64_t currentSequence;

    public:
        /** Returns the next valid sequence number or throws on exhaustion. */
        static SequenceNumber generate();
};

} // namespace lob

#endif // SEQUENCE_NUMBER_GENERATOR_HPP