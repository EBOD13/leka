#ifndef TIMESTAMP_HPP
#define TIMESTAMP_HPP

#include <cstdint>
#include <compare>

namespace lob {

class Timestamp {
    private:
        std::uint64_t timestamp;

    public:
        Timestamp(): timestamp(0){}

        explicit Timestamp(std::uint64_t timestamp): timestamp(timestamp){}

        std::uint64_t getTimestamp() const { return timestamp; }

        bool isValid() const {
            return timestamp != 0;
        }

        auto operator<=>(const Timestamp &other) const = default;
};

} // namespace lob

#endif // TIMESTAMP_HPP