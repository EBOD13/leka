#ifndef SEQUENCE_NUMBER_HPP
#define SEQUENCE_NUMBER_HPP
#include <cstdint>
#include <compare>

namespace lob {
/** @brief Monotonic engine sequence used for deterministic processing order. */
class SequenceNumber {
    private:
        std::uint64_t sequenceNumber;

    public:
        /** Constructs an invalid, zero-valued sequence number. */
        explicit SequenceNumber(): sequenceNumber(0){}

        /** Constructs a sequence number from its numeric value. */
        explicit SequenceNumber(std::uint64_t sequenceNumber): sequenceNumber(sequenceNumber){}

        /** Returns the underlying sequence value. */
        std::uint64_t getSequenceNumber() const { return sequenceNumber; }

        /** Returns whether the sequence number is nonzero. */
        bool isValid() const {
            return sequenceNumber != 0;
        };

        /** Compares sequence numbers by processing order. */
        auto operator<=>(const SequenceNumber &other) const = default;
};
} // namespace lob
#endif // SEQUENCE_NUMBER_HPP