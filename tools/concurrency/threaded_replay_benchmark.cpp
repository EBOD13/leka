// tools/concurrency/threaded_replay_benchmark.cpp

/**
 * @file
 * @brief Two-thread feed-handler / matching-engine pipeline, measured.
 *
 * tools/benchmark/benchmark_interchange.cpp replays an interchange CSV on a
 * single thread: parse, then call MatchingEngine::processEvent() directly in
 * a loop. This tool splits that into the shape a real venue-facing system
 * actually has: a feed-handler thread that decodes/produces events and a
 * matching thread that consumes and applies them, handed off through
 * SpscEventQueue (include/lob/concurrency/spsc_event_queue.hpp).
 *
 * Two latencies are reported, not one:
 *   HANDOFF     time from a successful tryPush() to the matching thread's
 *               tryPop() of that same event -- the cost concurrency itself
 *               introduces, and zero in the single-threaded tool by
 *               construction, since there is nothing to compare it to there.
 *   NEW/CANCEL/REDUCE  processEvent() latency, identical definition and
 *               category split to benchmark_interchange.cpp, so the two
 *               tools' numbers are directly comparable on the same input.
 *
 * The file is fully parsed into memory before either thread starts, exactly
 * as in the single-threaded tool, so parsing time is never part of what is
 * measured on either side of the queue.
 */

#include "lob/concurrency/spsc_event_queue.hpp"
#include "lob/matching/matching_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using lob::Execution;
using lob::MatchingEngine;
using lob::OrderBook;
using lob::OrderEvent;
using lob::OrderId;
using lob::OrderSide;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::SpscEventQueue;
using lob::Timestamp;

constexpr std::size_t ColumnCount = 8;
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
    while (i < s.size() && s[i] == ' ') ++i;
    std::uint64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + static_cast<std::uint64_t>(s[i] - '0');
    }
    return v;
}

bool parsePriceRaw(std::string_view s, std::uint64_t &out) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) return false;
    out = parseU64(s);
    return true;
}

struct MappedFile {
    const char *data = nullptr;
    std::size_t size = 0;
    int fd = -1;
    explicit MappedFile(const std::string &path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open " + path + ": " + std::strerror(errno));
        struct stat st{};
        if (::fstat(fd, &st) != 0) throw std::runtime_error("cannot stat " + path);
        size = static_cast<std::size_t>(st.st_size);
        void *m = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) throw std::runtime_error("mmap failed for " + path);
        data = static_cast<const char *>(m);
    }
    ~MappedFile() {
        if (data != nullptr) ::munmap(const_cast<char *>(data), size);
        if (fd >= 0) ::close(fd);
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
        if (end == std::string_view::npos) end = text.size();
        if (end > start) fn(text.substr(start, end - start));
        start = end + 1;
    }
}

enum class EventKind { New, Cancel, Reduce };

struct ParsedEvent {
    std::uint64_t tsNs = 0;
    EventKind kind = EventKind::New;
    std::uint64_t orderId = 0;
    OrderSide side = OrderSide::BUY;
    OrderType type = OrderType::LIMIT;
    std::uint64_t priceRaw = 0;
    std::uint64_t quantity = 0;
};

/** @brief Converts one parsed row into a real OrderEvent for the queue. */
OrderEvent toOrderEvent(const ParsedEvent &e) {
    const Timestamp timestamp{e.tsNs};
    const OrderId orderId{e.orderId};
    switch (e.kind) {
        case EventKind::New:
            return OrderEvent{lob::NewOrder{orderId, Price{e.priceRaw}, Quantity{e.quantity}, timestamp, e.side, e.type}};
        case EventKind::Cancel:
            return OrderEvent{lob::CancelOrder{orderId}};
        case EventKind::Reduce:
            return OrderEvent{lob::ReduceOrder{orderId, Quantity{e.quantity}}};
    }
    throw std::logic_error("unreachable");
}

class LatencyLog {
    public:
        void record(const char *label, std::uint64_t nanos) { samples[label].push_back(nanos); }
        void report() const {
            for (auto &[label, values] : samples) {
                if (values.empty()) continue;
                std::vector<std::uint64_t> sorted = values;
                std::ranges::sort(sorted);
                const auto pct = [&](double p) { return sorted[static_cast<std::size_t>(p * (sorted.size() - 1))]; };
                std::printf(
                    "  %-10s n=%-9zu min=%-6llu p50=%-6llu p90=%-6llu p99=%-6llu p99.9=%-6llu max=%llu (ns)\n",
                    label.c_str(), sorted.size(), static_cast<unsigned long long>(sorted.front()),
                    static_cast<unsigned long long>(pct(0.50)), static_cast<unsigned long long>(pct(0.90)),
                    static_cast<unsigned long long>(pct(0.99)), static_cast<unsigned long long>(pct(0.999)),
                    static_cast<unsigned long long>(sorted.back()));
            }
        }
    private:
        std::unordered_map<std::string, std::vector<std::uint64_t>> samples;
};

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: threaded_replay_benchmark FILE.csv [--headroom-pct N] [--tick N]\n");
        return 2;
    }
    std::string input = argv[1];
    double headroomPct = 20.0;
    std::uint64_t tick = 1;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (arg == "--headroom-pct") headroomPct = std::strtod(next().c_str(), nullptr);
        else if (arg == "--tick") tick = std::strtoull(next().c_str(), nullptr, 10);
    }

    MappedFile file(input);
    const std::string_view text = file.view();

    // --- Parse the whole file upfront; nothing below re-reads it, and
    // neither thread ever parses anything -- both start from the same
    // in-memory vector. ---
    std::vector<ParsedEvent> events;
    events.reserve(text.size() / 24);
    std::uint64_t minPriceRaw = UINT64_MAX, maxPriceRaw = 0;
    const auto noteRange = [&](std::uint64_t p) {
        if (p == 0) return;
        minPriceRaw = std::min(minPriceRaw, p);
        maxPriceRaw = std::max(maxPriceRaw, p);
    };
    bool firstLine = true;
    forEachLine(text, [&](std::string_view line) {
        if (firstLine) { firstLine = false; return; }
        const auto f = splitRow(line);
        ParsedEvent e;
        e.tsNs = parseU64(f[TsNs]);
        e.orderId = parseU64(f[OrderIdCol]);
        e.side = (!f[SideCol].empty() && f[SideCol][0] == 'B') ? OrderSide::BUY : OrderSide::SELL;
        e.type = (f[TypeCol] == "MARKET") ? OrderType::MARKET : OrderType::LIMIT;
        e.quantity = parseU64(f[QuantityCol]);
        if (f[EventCol] == "NEW") e.kind = EventKind::New;
        else if (f[EventCol] == "CANCEL") e.kind = EventKind::Cancel;
        else if (f[EventCol] == "REDUCE") e.kind = EventKind::Reduce;
        else return;
        std::uint64_t price = 0;
        if (parsePriceRaw(f[PriceCol], price)) {
            e.priceRaw = price;
            if (e.kind == EventKind::New && e.type == OrderType::LIMIT) noteRange(price);
        }
        events.push_back(e);
    });

    if (minPriceRaw == UINT64_MAX) {
        std::fprintf(stderr, "threaded_replay_benchmark: no priced LIMIT NEW rows found\n");
        return 1;
    }
    const std::uint64_t span = maxPriceRaw - minPriceRaw;
    const std::uint64_t rawHeadroom = static_cast<std::uint64_t>(static_cast<double>(span) * headroomPct / 100.0) + tick;
    const std::uint64_t headroom = ((rawHeadroom + tick - 1) / tick) * tick;
    std::uint64_t ladderMin = minPriceRaw > headroom ? minPriceRaw - headroom : minPriceRaw % tick;
    if (ladderMin == 0) ladderMin = tick;
    const std::uint64_t ladderMax = maxPriceRaw + headroom;
    const std::size_t levelCount = static_cast<std::size_t>((ladderMax - ladderMin) / tick) + 1;

    std::fprintf(stderr, "threaded_replay_benchmark: %s\n  events %zu, ladder levels %zu\n",
                 input.c_str(), events.size(), levelCount);

    OrderBook book(Price{ladderMin}, Price{tick}, levelCount);
    book.reserveOrderCapacity(events.size());
    MatchingEngine engine{book};

    // Queue capacity is deliberately much smaller than events.size(): the
    // point is to force the feed-handler thread to actually block on a full
    // queue sometimes (measuring real back-pressure), not to give it enough
    // room to run the whole file ahead of the matching thread uncontested.
    static constexpr std::size_t QueueCapacity = 4096;
    SpscEventQueue<QueueCapacity> queue;

    // atomic<uint64_t>, not plain uint64_t: pushNs[i] is written by the
    // feed-handler thread and read by the matching thread, and — this was
    // caught by actually running this tool, not by inspection — a plain
    // std::vector<uint64_t> here is a genuine data race. tryPush()'s own
    // internal release-store only publishes what was written BEFORE it in
    // program order; a timestamp recorded AFTER tryPush() returns has no
    // happens-before relationship to the consumer's later read of it at
    // all. In practice that surfaced as a torn/stale read producing an
    // apparent HANDOFF max of several million years. Each element gets its
    // own release (producer) / acquire (consumer) pair here instead, which
    // is a second, independent synchronization edge from the queue's own.
    std::vector<std::atomic<std::uint64_t>> pushNs(events.size());
    std::vector<std::uint64_t> popNs(events.size());

    const auto nowNs = [] {
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    };

    std::thread feedHandler([&] {
        for (std::size_t i = 0; i < events.size(); ++i) {
            const OrderEvent event = toOrderEvent(events[i]);
            while (!queue.tryPush(event)) {
                std::this_thread::yield();
            }
            pushNs[i].store(nowNs(), std::memory_order_release);
        }
    });

    LatencyLog latency;
    std::vector<Execution> executionBuffer;
    std::size_t consumed = 0;
    std::uint64_t errorCount = 0;
    const auto pipelineStart = std::chrono::steady_clock::now();

    while (consumed < events.size()) {
        OrderEvent event{lob::CancelOrder{OrderId{}}}; // overwritten by tryPop on success
        if (!queue.tryPop(event)) {
            std::this_thread::yield();
            continue;
        }
        const std::uint64_t poppedAt = nowNs();
        popNs[consumed] = poppedAt;
        const std::uint64_t pushedAt = pushNs[consumed].load(std::memory_order_acquire);
        latency.record("HANDOFF", poppedAt - pushedAt);

        const ParsedEvent &raw = events[consumed];
        try {
            const auto start = std::chrono::steady_clock::now();
            engine.processEvent(event, executionBuffer);
            const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());

            const char *label = "CANCEL";
            if (raw.kind == EventKind::New) {
                label = (raw.type == OrderType::MARKET || !executionBuffer.empty()) ? "NEW_CROSS" : "NEW_REST";
            } else if (raw.kind == EventKind::Reduce) {
                label = "REDUCE";
            }
            latency.record(label, elapsed);
        } catch (const std::exception &) {
            // Matches benchmark_interchange.cpp's handling: a REDUCE or
            // CANCEL referencing an order the book no longer has (or never
            // had, if this CSV came from a source not perfectly consistent
            // with a fresh book) is a data issue, not a queue or threading
            // bug, and is counted rather than allowed to crash the pipeline.
            ++errorCount;
        }
        ++consumed;
    }

    feedHandler.join();
    const auto pipelineElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - pipelineStart).count();

    std::printf("threaded_replay_benchmark: %zu events replayed over %.1f ms (%.0f events/sec, two threads), %llu errors\n",
                events.size(), static_cast<double>(pipelineElapsed) / 1e6,
                static_cast<double>(events.size()) / (static_cast<double>(pipelineElapsed) / 1e9),
                static_cast<unsigned long long>(errorCount));
    latency.report();
    return 0;
}
