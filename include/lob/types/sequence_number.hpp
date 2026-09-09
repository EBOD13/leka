#ifndef SEQUENCE_NUMBER_HPP
#define SEQUENCE_NUMBER_HPP
#include <cstdint>
#include <compare>

namespace lob {
class SequenceNumber {
    private:
        std::uint64_t sequenceNumber;

    public:
        explicit SequenceNumber(): sequenceNumber(0){} // Default constructor initializes at 0

        explicit SequenceNumber(std::uint64_t sequenceNumber): sequenceNumber(sequenceNumber){} // Assign the sequence number to the order

        // Getter for the sequence number
        std::uint64_t getSequenceNumber() const { return sequenceNumber; }

        // Make sure the sequence number is valid (not 0)
        bool isValid() const {
            return sequenceNumber != 0;
        };

        auto operator<=>(const SequenceNumber &other) const = default;
};
} // namespace lob
#endif // SEQUENCE_NUMBER_HPP