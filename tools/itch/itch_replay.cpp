// tools/itch/itch_replay.cpp

/**
 * @file
 * @brief Replays an itch_to_csv CSV through Leka's matching engine.
 *
 * This closes the loop from real Nasdaq TotalView-ITCH data to a correctly
 * sized lob::OrderBook: it scans the file once to determine the price range
 * actually traded, constructs the book's PriceLadder to fit that range plus
 * headroom, then replays the session in order.
 *
 * Event mapping, matching README.md:
 *   ADD_ORDER / ADD_ORDER_MPID  -> NEW (resting limit order)
 *   DELETE                      -> CANCEL
 *   CANCEL_PARTIAL              -> REDUCE (ITCH gives shares cancelled, not
 *                                  remaining; the replayed quantity is
 *                                  computed from the book's own live state)
 *   REPLACE                     -> CANCEL(old ref) then NEW(new ref); ITCH
 *                                  has no in-place modify, so neither does
 *                                  Leka's event model
 *
 * EXECUTED / EXECUTED_WITH_PRICE carry no order of their own: ITCH never
 * publishes the aggressor as a message, only its effect on the resting side.
 * They are reconstructed here exactly as validated by hand against
 * 20190730.BX_ITCH_50 (2,154/2,154 executions grouped, 99.2% single-level):
 * group executions sharing an identical timestamp and resting side, then
 * inject one synthetic NEW on the opposite side, with quantity equal to the
 * summed executed shares and price equal to the worst price paid in the
 * group, an observed bound rather than a modeled one. TRADE_NON_CROSS,
 * TRADE_CROSS, and TRADE_BROKEN never touched the displayed book and are not
 * replayed.
 *
 * CAVEAT: because CANCEL_PARTIAL's replayed quantity and REPLACE's replayed
 * side are read from Leka's own live book rather than from an independent
 * reconstruction, any earlier desync between the replay and the real venue
 * (there should be none, since executions are reconstructed) would compound.
 * The desync counters this tool prints at the end are the way to check that
 * assumption rather than trust it silently.
 */

#include "lob/matching/matching_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

constexpr std::size_t ColumnCount = 15;
// seq,ts_ns,epoch_ns,clock_et,msg,name,symbol,order_ref,side,shares,price,
// new_order_ref,match_number,printable,attribution
enum Column {
    Seq = 0, TsNs = 1, EpochNs = 2, ClockEt = 3, Msg = 4, Name = 5, Symbol = 6,
    OrderRef = 7, Side = 8, Shares = 9, PriceCol = 10, NewOrderRef = 11,
    MatchNumber = 12, Printable = 13, Attribution = 14
};

/** @brief Splits one CSV line into its fixed 15 fields without allocating. */
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

/** @brief Parses an unsigned decimal field; empty input yields 0. */
std::uint64_t parseU64(std::string_view s) {
    std::uint64_t v = 0;
    for (char c : s) {
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

/** @brief Parses a "123.4567"-style price field into raw 4-decimal ticks. */
bool parsePriceRaw(std::string_view s, std::uint64_t &out) {
    if (s.empty()) {
        return false;
    }
    const auto dot = s.find('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    out = parseU64(s.substr(0, dot)) * 10000 + parseU64(s.substr(dot + 1));
    return true;
}

/** @brief Memory-maps a file read-only and exposes it as one string_view. */
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

/** @brief Invokes fn(line) for every non-empty line in text, in order. */
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

/** @brief One reconstructed aggressor: never present as its own ITCH message. */
struct AggressorEvent {
    OrderSide side;
    std::uint64_t quantity;
    std::uint64_t priceRaw;
};

/** @brief Accumulates one (timestamp, resting side) execution group. */
struct Aggregate {
    char restingSide = 'B';
    std::uint64_t quantity = 0;
    std::uint64_t priceExtreme = 0;
    bool started = false;
};

/** @brief Per-event-type latency samples, reported as percentiles at the end. */
class LatencyLog {
    public:
        void record(const char *label, std::uint64_t nanos) {
            samples[label].push_back(nanos);
        }

        void report() const {
            for (auto &[label, values] : samples) {
                if (values.empty()) {
                    continue;
                }
                std::vector<std::uint64_t> sorted = values;
                std::sort(sorted.begin(), sorted.end());
                const auto pct = [&](double p) {
                    std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1));
                    return sorted[idx];
                };
                std::fprintf(stderr,
                    "  %-8s n=%-9zu p50=%-6llu p90=%-6llu p99=%-6llu p99.9=%-6llu max=%llu (ns)\n",
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

/** @brief Command-line configuration. */
struct Options {
    std::string input;
    double headroomPct = 20.0; // extra range on each side of the observed min/max
    std::uint64_t tick = 1;    // matches the CSV's native 4-implied-decimal raw units
};

void usage() {
    std::fprintf(stderr,
        "usage: itch_replay --input FILE.csv [--headroom-pct N] [--tick N]\n"
        "\n"
        "  FILE.csv is per-symbol output from itch_to_csv.\n");
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            return i + 1 < argc ? argv[++i] : std::string{};
        };
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

    // --- Pass 1: determine the price range and reconstruct aggressors ---
    std::unordered_map<std::uint64_t, char> refSide;
    std::unordered_map<std::uint64_t, std::uint64_t> refPrice;
    std::unordered_map<std::uint64_t, Aggregate> aggregates; // key: ts_ns*2 + (restingSide=='S')
    std::uint64_t minPriceRaw = UINT64_MAX;
    std::uint64_t maxPriceRaw = 0;
    std::uint64_t rowCount = 0;

    const auto noteRange = [&](std::uint64_t p) {
        if (p == 0) {
            return;
        }
        minPriceRaw = std::min(minPriceRaw, p);
        maxPriceRaw = std::max(maxPriceRaw, p);
    };

    bool firstLine = true;
    forEachLine(text, [&](std::string_view line) {
        if (firstLine) {
            firstLine = false;
            return; // header
        }
        ++rowCount;
        const auto f = splitRow(line);
        if (f[Msg].size() != 1) {
            return;
        }
        const char msg = f[Msg][0];
        const std::uint64_t ref = parseU64(f[OrderRef]);

        if (msg == 'A' || msg == 'F') {
            std::uint64_t price = 0;
            parsePriceRaw(f[PriceCol], price);
            refSide[ref] = f[Side].empty() ? 'B' : f[Side][0];
            refPrice[ref] = price;
            noteRange(price);
        } else if (msg == 'U') {
            const std::uint64_t newRef = parseU64(f[NewOrderRef]);
            std::uint64_t price = 0;
            parsePriceRaw(f[PriceCol], price);
            const char side = refSide.count(ref) != 0 ? refSide[ref] : 'B';
            refSide.erase(ref);
            refPrice.erase(ref);
            refSide[newRef] = side;
            refPrice[newRef] = price;
            noteRange(price);
        } else if (msg == 'D') {
            refSide.erase(ref);
            refPrice.erase(ref);
        } else if (msg == 'E' || msg == 'C') {
            const auto sideIt = refSide.find(ref);
            if (sideIt == refSide.end()) {
                return; // order added before this file's window began
            }
            const char restingSide = sideIt->second;
            const std::uint64_t qty = parseU64(f[Shares]);
            std::uint64_t price = 0;
            if (msg == 'C') {
                parsePriceRaw(f[PriceCol], price);
            } else {
                price = refPrice.count(ref) != 0 ? refPrice[ref] : 0;
            }
            const std::uint64_t ts = parseU64(f[TsNs]);
            const std::uint64_t key = ts * 2 + (restingSide == 'S' ? 1 : 0);
            Aggregate &agg = aggregates[key];
            if (!agg.started) {
                agg.started = true;
                agg.restingSide = restingSide;
                agg.priceExtreme = price;
            } else if (restingSide == 'S') {
                agg.priceExtreme = std::max(agg.priceExtreme, price); // buy aggressor: worst = highest paid
            } else {
                agg.priceExtreme = std::min(agg.priceExtreme, price); // sell aggressor: worst = lowest accepted
            }
            agg.quantity += qty;
            noteRange(agg.priceExtreme);
        }
    });

    // Fold the aggregated groups into a ts_ns -> events lookup for replay.
    std::unordered_map<std::uint64_t, std::vector<AggressorEvent>> aggressorsByTs;
    for (auto &[key, agg] : aggregates) {
        const std::uint64_t ts = key / 2;
        aggressorsByTs[ts].push_back(AggressorEvent{
            agg.restingSide == 'S' ? OrderSide::BUY : OrderSide::SELL,
            agg.quantity, agg.priceExtreme});
    }

    if (minPriceRaw == UINT64_MAX) {
        std::fprintf(stderr, "itch_replay: no priced ADD_ORDER rows found\n");
        return 1;
    }

    const std::uint64_t span = maxPriceRaw - minPriceRaw;
    const std::uint64_t headroom =
        static_cast<std::uint64_t>(static_cast<double>(span) * options.headroomPct / 100.0) + options.tick;
    const std::uint64_t ladderMin = minPriceRaw > headroom ? minPriceRaw - headroom : options.tick;
    const std::uint64_t ladderMax = maxPriceRaw + headroom;
    const std::size_t levelCount =
        static_cast<std::size_t>((ladderMax - ladderMin) / options.tick) + 1;

    std::fprintf(stderr,
        "itch_replay: %s\n"
        "  rows scanned        %llu\n"
        "  observed price range %.4f .. %.4f\n"
        "  ladder configured    %.4f .. %.4f, tick=%llu, levels=%zu (%.2f MB per side)\n"
        "  reconstructed aggressors %zu\n",
        options.input.c_str(), static_cast<unsigned long long>(rowCount),
        static_cast<double>(minPriceRaw) / 10000.0, static_cast<double>(maxPriceRaw) / 10000.0,
        static_cast<double>(ladderMin) / 10000.0, static_cast<double>(ladderMax) / 10000.0,
        static_cast<unsigned long long>(options.tick), levelCount,
        static_cast<double>(levelCount) * sizeof(lob::PriceLevel) / 1e6,
        aggregates.size());

    // --- Pass 2: replay through a correctly sized book ---
    OrderBook book(Price{ladderMin}, Price{options.tick}, levelCount);
    // rowCount is a generous upper bound on resting inserts (every row is at
    // most one NEW), plus the reconstructed aggressors. Sizing this up front
    // avoids OrderPool and OrderIndex allocating or rehashing mid-replay; see
    // ARCH_DECISIONS.md ADR-008.
    book.reserveOrderCapacity(rowCount + aggregates.size());
    MatchingEngine engine{book};
    LatencyLog latency;
    // Reused across every NEW and reconstructed aggressor below via
    // MatchingEngine's buffer-taking overload (see ARCH_DECISIONS.md), so
    // this replay pays for at most one execution-vector allocation total
    // rather than one per aggressive order.
    std::vector<Execution> executionBuffer;

    constexpr std::uint64_t SyntheticIdBase = 1ULL << 62;
    std::uint64_t syntheticIdCounter = 0;
    std::unordered_map<std::string, std::uint64_t> eventCounts;
    std::uint64_t reduceDesync = 0;
    std::uint64_t replaceDesync = 0;

    const auto timed = [&](const char *label, auto &&fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        latency.record(label, static_cast<std::uint64_t>(elapsed));
    };

    firstLine = true;
    forEachLine(text, [&](std::string_view line) {
        if (firstLine) {
            firstLine = false;
            return;
        }
        const auto f = splitRow(line);
        if (f[Msg].size() != 1) {
            return;
        }
        const std::uint64_t ts = parseU64(f[TsNs]);
        const std::uint64_t epochNs = parseU64(f[EpochNs]);
        const Timestamp timestamp{epochNs != 0 ? epochNs : ts};

        // Inject any reconstructed aggressor orders due at this exact timestamp,
        // exactly once, before processing the row that carries that timestamp.
        auto aggIt = aggressorsByTs.find(ts);
        if (aggIt != aggressorsByTs.end()) {
            for (const auto &agg : aggIt->second) {
                const OrderId id{SyntheticIdBase + syntheticIdCounter++};
                try {
                    timed("AGGR", [&] {
                        engine.processEvent(OrderEvent{NewOrder{
                            id, Price{agg.priceRaw}, Quantity{agg.quantity},
                            timestamp, agg.side, OrderType::LIMIT}}, executionBuffer);
                    });
                    ++eventCounts["AGGR"];
                } catch (const std::exception &) {
                    ++eventCounts["AGGR_FAILED"];
                }
            }
            aggressorsByTs.erase(aggIt);
        }

        const char msg = f[Msg][0];
        const std::uint64_t ref = parseU64(f[OrderRef]);

        try {
            if (msg == 'A' || msg == 'F') {
                std::uint64_t price = 0;
                parsePriceRaw(f[PriceCol], price);
                const OrderSide side = (!f[Side].empty() && f[Side][0] == 'B') ? OrderSide::BUY : OrderSide::SELL;
                timed("NEW", [&] {
                    engine.processEvent(OrderEvent{NewOrder{
                        OrderId{ref}, Price{price}, Quantity{parseU64(f[Shares])},
                        timestamp, side, OrderType::LIMIT}}, executionBuffer);
                });
                ++eventCounts["NEW"];
            } else if (msg == 'D') {
                timed("CANCEL", [&] {
                    engine.processEvent(OrderEvent{CancelOrder{OrderId{ref}}});
                });
                ++eventCounts["CANCEL"];
            } else if (msg == 'X') {
                const lob::Order *existing = book.findOrder(OrderId{ref});
                if (existing != nullptr) {
                    const std::uint64_t remaining = existing->getRemainingQuantity().getQuantity();
                    const std::uint64_t cancelled = parseU64(f[Shares]);
                    if (cancelled < remaining) {
                        timed("REDUCE", [&] {
                            engine.processEvent(OrderEvent{ReduceOrder{
                                OrderId{ref}, Quantity{remaining - cancelled}}});
                        });
                        ++eventCounts["REDUCE"];
                    } else {
                        ++reduceDesync;
                        timed("CANCEL", [&] {
                            engine.processEvent(OrderEvent{CancelOrder{OrderId{ref}}});
                        });
                        ++eventCounts["CANCEL"];
                    }
                }
            } else if (msg == 'U') {
                const lob::Order *existing = book.findOrder(OrderId{ref});
                if (existing != nullptr) {
                    const OrderSide side = existing->getOrderSide();
                    const std::uint64_t newRef = parseU64(f[NewOrderRef]);
                    std::uint64_t price = 0;
                    parsePriceRaw(f[PriceCol], price);
                    timed("CANCEL", [&] {
                        engine.processEvent(OrderEvent{CancelOrder{OrderId{ref}}});
                    });
                    ++eventCounts["CANCEL"];
                    timed("NEW", [&] {
                        engine.processEvent(OrderEvent{NewOrder{
                            OrderId{newRef}, Price{price}, Quantity{parseU64(f[Shares])},
                            timestamp, side, OrderType::LIMIT}}, executionBuffer);
                    });
                    ++eventCounts["NEW"];
                } else {
                    ++replaceDesync;
                }
            }
            // E, C, P, Q, B, S, H: no direct action; E/C already represented
            // through the aggressor injection above.
        } catch (const std::exception &e) {
            ++eventCounts[std::string("ERROR:") + msg];
        }
    });

    std::fprintf(stderr,
        "\nreplay complete\n"
        "  events replayed:");
    for (auto &[label, count] : eventCounts) {
        std::fprintf(stderr, " %s=%llu", label.c_str(), static_cast<unsigned long long>(count));
    }
    std::fprintf(stderr,
        "\n  REDUCE-to-full-cancel desyncs (X where cancelled >= live remaining): %llu\n"
        "  REPLACE with no live old order (U with unknown old ref):            %llu\n"
        "  resting levels remaining: bid=%zu ask=%zu (nonzero is expected: this\n"
        "  tool does not model resting orders consumed by non-displayed liquidity)\n\n"
        "latency by event type (matching engine dispatch only):\n",
        static_cast<unsigned long long>(reduceDesync),
        static_cast<unsigned long long>(replaceDesync),
        book.getBidLevelCount(), book.getAskLevelCount());
    latency.report();

    return 0;
}
