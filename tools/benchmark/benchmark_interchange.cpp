// tools/benchmark/benchmark_interchange.cpp

/**
 * @file
 * @brief Per-event-type latency percentiles for MatchingEngine::processEvent,
 * driven by the interchange CSV schema (seq,ts_ns,event,order_id,side,type,
 * price,quantity; event in NEW/CANCEL/REDUCE; price is an integer in
 * 1/10000 units, matching Price's own raw representation directly).
 *
 * This is deliberately not benchmark_main.cpp, which drives a synthetic
 * random workload generated in-process. This tool replays an externally
 * produced event log -- real ITCH data translated to the interchange
 * schema, or a simulator's output -- so the same measurement can be run
 * against different sources of order flow and compared.
 *
 * Methodology:
 *   1. The whole file is parsed into an in-memory vector of ParsedEvent
 *      first. Nothing below that point touches the CSV again, so parsing
 *      time is never part of what gets timed.
 *   2. Only the steady_clock interval around processEvent() is measured.
 *   3. NEW is split into NEW_REST and NEW_CROSS: a LIMIT NEW is classified
 *      after the call by whether it produced any executions (checked on
 *      the reused execution buffer, itself outside the timed interval); a
 *      MARKET NEW is always NEW_CROSS, since it can never rest by
 *      definition, whether or not it actually finds liquidity.
 *   4. Percentiles (p50/p90/p99/p99.9/max), never a mean, are reported per
 *      category -- a mean hides exactly the tail a latency-sensitive
 *      system cares about.
 */

#include "lob/matching/matching_engine.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using lob::CancelOrder;
using lob::Execution;
using lob::MatchingEngine;
using lob::NewOrder;
using lob::OrderBook;
using lob::OrderEvent;
using lob::OrderId;
using lob::OrderSide;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::ReduceOrder;
using lob::Timestamp;

constexpr std::size_t ColumnCount = 8;
// seq,ts_ns,event,order_id,side,type,price,quantity
enum Column { Seq = 0, TsNs = 1, EventCol = 2, OrderIdCol = 3, SideCol = 4, TypeCol = 5, PriceCol = 6, QuantityCol = 7 };

std::array<std::string_view, ColumnCount> splitRow(std::string_view line) {
    std::array<std::string_view, ColumnCount> fields{};
    std::size_t fieldIndex = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size() && fieldIndex < ColumnCount; ++i) {
        if (i == line.size() || line[i] == ',') {
            fields[fieldIndex++] = line.substr(start, i - start);
            start = i + 1;
        }
    }
    return fields;
}

std::uint64_t parseU64(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') {
        ++i; // some CSV writers right-align numeric columns with leading spaces
    }
    std::uint64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
            break; // tolerate trailing whitespace too
        }
        v = v * 10 + static_cast<std::uint64_t>(s[i] - '0');
    }
    return v;
}

/** @brief Parses a plain-integer price field (already 1/10000-unit raw ticks). */
bool parsePriceRaw(std::string_view s, std::uint64_t &out) {
    // Trim surrounding whitespace some CSV writers pad numeric columns with.
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) {
        return false; // MARKET orders: price column is blank
    }
    out = parseU64(s);
    return true;
}

struct MappedFile {
    const char *data = nullptr;
    std::size_t size = 0;
    int fd = -1;

    explicit MappedFile(const std::string &path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
        }
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            throw std::runtime_error("cannot stat " + path);
        }
        size = static_cast<std::size_t>(st.st_size);
        void *m = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) {
            throw std::runtime_error("mmap failed for " + path);
        }
        data = static_cast<const char *>(m);
    }
    ~MappedFile() {
        if (data != nullptr) {
            ::munmap(const_cast<char *>(data), size);
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
    std::string_view view() const { return {data, size}; }
};

template <typename Fn>
void forEachLine(std::string_view text, Fn &&fn) {
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (end > start) {
            fn(text.substr(start, end - start));
        }
        start = end + 1;
    }
}

enum class EventKind { New, Cancel, Reduce };

/** @brief One fully-parsed interchange row, ready to replay with no further parsing. */
struct ParsedEvent {
    std::uint64_t tsNs = 0;
    EventKind kind = EventKind::New;
    std::uint64_t orderId = 0;
    OrderSide side = OrderSide::BUY;
    OrderType type = OrderType::LIMIT;
    std::uint64_t priceRaw = 0;
    std::uint64_t quantity = 0;
};

class LatencyLog {
    public:
        void record(const char *label, std::uint64_t nanos) { samples[label].push_back(nanos); }

        void report() const {
            for (auto &[label, values] : samples) {
                if (values.empty()) {
                    continue;
                }
                std::vector<std::uint64_t> sorted = values;
                std::sort(sorted.begin(), sorted.end());
                const auto pct = [&](double p) {
                    const std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1));
                    return sorted[idx];
                };
                std::printf(
                    "  %-10s n=%-9zu p50=%-6llu p90=%-6llu p99=%-6llu p99.9=%-6llu max=%llu (ns)\n",
                    label.c_str(), sorted.size(),
                    static_cast<unsigned long long>(pct(0.50)),
                    static_cast<unsigned long long>(pct(0.90)),
                    static_cast<unsigned long long>(pct(0.99)),
                    static_cast<unsigned long long>(pct(0.999)),
                    static_cast<unsigned long long>(sorted.back()));
            }
        }

    private:
        std::unordered_map<std::string, std::vector<std::uint64_t>> samples;
};

struct Options {
    std::string input;
    double headroomPct = 20.0;
    std::uint64_t tick = 1;
};

void usage() {
    std::fprintf(stderr,
        "usage: benchmark_interchange --input FILE.csv [--headroom-pct N] [--tick N]\n"
        "\n"
        "  FILE.csv is in the interchange schema: seq,ts_ns,event,order_id,\n"
        "  side,type,price,quantity (event in NEW/CANCEL/REDUCE, price as an\n"
        "  integer in 1/10000 units). --tick must match the price ladder's\n"
        "  actual tick spacing in those same raw units (100 for a one-cent\n"
        "  tick under this schema's convention).\n");
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (arg == "--input") {
            options.input = next();
        } else if (arg == "--headroom-pct") {
            options.headroomPct = std::strtod(next().c_str(), nullptr);
        } else if (arg == "--tick") {
            options.tick = std::strtoull(next().c_str(), nullptr, 10);
        } else {
            usage();
            return 2;
        }
    }
    if (options.input.empty()) {
        usage();
        return 2;
    }

    MappedFile file(options.input);
    const std::string_view text = file.view();

    // --- Pass 1: parse every row into memory. Nothing timed here, and
    // nothing below this point re-reads the file. ---
    std::vector<ParsedEvent> events;
    events.reserve(text.size() / 24); // rough: interchange rows run ~20-30 bytes each

    std::uint64_t minPriceRaw = UINT64_MAX;
    std::uint64_t maxPriceRaw = 0;
    const auto noteRange = [&](std::uint64_t p) {
        if (p == 0) return;
        minPriceRaw = std::min(minPriceRaw, p);
        maxPriceRaw = std::max(maxPriceRaw, p);
    };

    bool firstLine = true;
    std::uint64_t rowCount = 0;
    std::uint64_t skipped = 0;
    forEachLine(text, [&](std::string_view line) {
        if (firstLine) {
            firstLine = false;
            return; // header
        }
        ++rowCount;
        const auto f = splitRow(line);

        ParsedEvent e;
        e.tsNs = parseU64(f[TsNs]);
        e.orderId = parseU64(f[OrderIdCol]);
        e.side = (!f[SideCol].empty() && f[SideCol][0] == 'B') ? OrderSide::BUY : OrderSide::SELL;
        e.type = (f[TypeCol] == "MARKET") ? OrderType::MARKET : OrderType::LIMIT;
        e.quantity = parseU64(f[QuantityCol]);

        if (f[EventCol] == "NEW") {
            e.kind = EventKind::New;
        } else if (f[EventCol] == "CANCEL") {
            e.kind = EventKind::Cancel;
        } else if (f[EventCol] == "REDUCE") {
            e.kind = EventKind::Reduce;
        } else {
            ++skipped;
            return;
        }

        std::uint64_t price = 0;
        if (parsePriceRaw(f[PriceCol], price)) {
            e.priceRaw = price;
            if (e.kind == EventKind::New && e.type == OrderType::LIMIT) {
                noteRange(price);
            }
        }

        events.push_back(e);
    });

    if (minPriceRaw == UINT64_MAX) {
        std::fprintf(stderr, "benchmark_interchange: no priced LIMIT NEW rows found\n");
        return 1;
    }

    // PriceLadder::levelAt() requires every queried price to land exactly on
    // minPrice + k*tick (see price_ladder.hpp); rounding headroom up to a
    // tick multiple keeps ladderMin on minPriceRaw's own tick residue, so
    // every observed price stays on-grid. An unrounded headroom (the
    // straightforward-looking version of this calculation) silently shifts
    // ladderMin off that grid whenever tick != 1, and every single lookup
    // then throws std::invalid_argument -- caught here once, the hard way.
    const std::uint64_t span = maxPriceRaw - minPriceRaw;
    const std::uint64_t rawHeadroom =
        static_cast<std::uint64_t>(static_cast<double>(span) * options.headroomPct / 100.0) + options.tick;
    const std::uint64_t headroom = ((rawHeadroom + options.tick - 1) / options.tick) * options.tick;
    std::uint64_t ladderMin = minPriceRaw > headroom ? minPriceRaw - headroom : minPriceRaw % options.tick;
    if (ladderMin == 0) {
        ladderMin = options.tick; // Price{0} is reserved as invalid
    }
    const std::uint64_t ladderMax = maxPriceRaw + headroom;
    const std::size_t levelCount = static_cast<std::size_t>((ladderMax - ladderMin) / options.tick) + 1;

    std::fprintf(stderr,
        "benchmark_interchange: %s\n"
        "  rows parsed          %llu (skipped %llu non-NEW/CANCEL/REDUCE)\n"
        "  events materialized  %zu\n"
        "  observed price range %.4f .. %.4f\n"
        "  ladder configured    %.4f .. %.4f, tick=%llu, levels=%zu (%.2f MB per side)\n",
        options.input.c_str(), static_cast<unsigned long long>(rowCount),
        static_cast<unsigned long long>(skipped), events.size(),
        static_cast<double>(minPriceRaw) / 10000.0, static_cast<double>(maxPriceRaw) / 10000.0,
        static_cast<double>(ladderMin) / 10000.0, static_cast<double>(ladderMax) / 10000.0,
        static_cast<unsigned long long>(options.tick), levelCount,
        static_cast<double>(levelCount) * sizeof(lob::PriceLevel) / 1e6);

    // --- Pass 2: the timed replay ---
    OrderBook book(Price{ladderMin}, Price{options.tick}, levelCount);
    MatchingEngine engine{book};
    LatencyLog latency;
    std::vector<Execution> executionBuffer; // reused across every call; see ARCH_DECISIONS.md

    std::unordered_map<std::string, std::uint64_t> eventCounts;
    std::uint64_t errorCount = 0;

    // Returns elapsed nanoseconds without recording anything: classifying a
    // NEW into REST/CROSS needs the call's own outcome (whether it produced
    // executions), which is only known after the call returns, so the label
    // cannot be chosen up front. Timing itself still brackets processEvent()
    // alone -- fn() is the entire measured interval, nothing else.
    const auto timed = [&](auto &&fn) -> std::uint64_t {
        const auto start = std::chrono::steady_clock::now();
        fn();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
    };

    const auto benchmarkStart = std::chrono::steady_clock::now();

    for (const ParsedEvent &e : events) {
        const Timestamp timestamp{e.tsNs};
        const OrderId orderId{e.orderId};
        try {
            if (e.kind == EventKind::New) {
                const Price price{e.priceRaw};
                const std::uint64_t elapsed = timed([&] {
                    engine.processEvent(
                        OrderEvent{NewOrder{orderId, price, Quantity{e.quantity}, timestamp, e.side, e.type}},
                        executionBuffer);
                });
                // executionBuffer.empty() is read after the timed call
                // returns, so this check itself is never part of what's
                // measured. A MARKET order is always NEW_CROSS by
                // definition (it can never rest), whether or not it found
                // liquidity to match against.
                const char *label =
                    (e.type == OrderType::MARKET || !executionBuffer.empty()) ? "NEW_CROSS" : "NEW_REST";
                latency.record(label, elapsed);
                ++eventCounts[label];
            } else if (e.kind == EventKind::Cancel) {
                const std::uint64_t elapsed = timed([&] {
                    engine.processEvent(OrderEvent{CancelOrder{orderId}});
                });
                latency.record("CANCEL", elapsed);
                ++eventCounts["CANCEL"];
            } else {
                const std::uint64_t elapsed = timed([&] {
                    engine.processEvent(OrderEvent{ReduceOrder{orderId, Quantity{e.quantity}}});
                });
                latency.record("REDUCE", elapsed);
                ++eventCounts["REDUCE"];
            }
        } catch (const std::exception &) {
            ++errorCount;
        }
    }

    const auto totalElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - benchmarkStart).count();

    std::printf("benchmark_interchange: %zu events replayed over %.1f ms (%.0f events/sec), %llu errors\n",
                events.size(), static_cast<double>(totalElapsed) / 1e6,
                static_cast<double>(events.size()) / (static_cast<double>(totalElapsed) / 1e9),
                static_cast<unsigned long long>(errorCount));
    for (auto &[label, count] : eventCounts) {
        std::printf("  %s=%llu", label.c_str(), static_cast<unsigned long long>(count));
    }
    std::printf("\n\nlatency by event type (matching engine dispatch only):\n");
    latency.report();

    return 0;
}
