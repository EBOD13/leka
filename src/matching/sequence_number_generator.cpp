#include "lob/matching/sequence_number_generator.hpp"

#include <limits>
#include <stdexcept>

namespace lob {

std::uint64_t SequenceNumberGenerator::currentSequence = 0;

SequenceNumber SequenceNumberGenerator::generate() {
    if (currentSequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Sequence number space exhausted");
    }

    return SequenceNumber{++currentSequence};
}

} // namespace lob