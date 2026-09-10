#ifndef TIMESTAMP_HPP
#define TIMESTAMP_HPP

#include <cstdint>
#include <compare>

namespace lob {

/** @brief Nanoseconds since the Unix epoch; zero is reserved as invalid. */
class Timestamp {
    private:
        std::uint64_t timestamp;

    public:
        /** Constructs an invalid, zero-valued timestamp. */
        Timestamp(): timestamp(0){}

        /** Constructs a timestamp from nanoseconds since the Unix epoch. */
        explicit Timestamp(std::uint64_t timestamp): timestamp(timestamp){}

        /** Returns nanoseconds since the Unix epoch. */
        std::uint64_t getTimestamp() const { return timestamp; }

        /** Returns whether the timestamp is nonzero. */
        bool isValid() const {
            return timestamp != 0;
        }

        /** Compares timestamps chronologically. */
        auto operator<=>(const Timestamp &other) const = default;
};

} // namespace lob

#endif // TIMESTAMP_HPP