// tools/viz/snapshot_recorder.cpp

/**
 * @file
 * @brief Replays an interchange CSV and records periodic book snapshots as JSONL.
 *
 * This is the offline half of the visualization: it produces a file a static
 * web page can animate, with no server involved. The record schema is
 * deliberately the same shape a live WebSocket server would publish
 * (lob::BookSnapshot plus the rolling counters a viewer needs), so the
 * browser code written against a recorded file works unchanged against a
 * live feed later.
 *
 * Hot-path discipline: the snapshot is captured *between* processEvent()
 * calls and the JSON is serialized outside the timed region, so nothing the
 * viewer needs executes inside the matching path. The latency figures
 * recorded here are measured exactly as tools/benchmark/benchmark_interchange
 * measures them, so the two are comparable. See ARCH_DECISIONS.md ADR-010.
 *
 * One JSON object per line:
 *   seq   events applied so far          ts    engine timestamp (ns)
 *   bb/ba best bid / best ask, raw       nb/na occupied level count per side
 *   bids/asks  [[priceRaw, qty, orders], ...] best first
 *   flow  cumulative event and volume counters
 *   lat   rolling [p50, p99, p999] per event type, nanoseconds
 *   tape  executions since the previous snapshot: [ts, priceRaw, qty, aggressorSide]
 */

#include "lob/matching/matching_engine.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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

using lob::BookSnapshot;
using lob::Execution;
using lob::MatchingEngine;
using lob::OrderBook;
using lob::OrderEvent;
using lob::OrderId;
using lob::OrderSide;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::Timestamp;

constexpr std::size_t ColumnCount = 8;
enum Column { Seq = 0, TsNs = 1, EventCol = 2, OrderIdCol = 3, SideCol = 4, TypeCol = 5, PriceCol = 6, QuantityCol = 7 };

std::array<std::string_view, ColumnCount> splitRow(std::string_view line) {
    std::array<std::string_view, ColumnCount> fields{};
    std::size_t fieldIndex = 0, start = 0;
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

/**
 * @brief Fixed-size ring of recent latency samples, summarized on demand.
 *
 * A bounded window rather than the whole run, so the viewer shows latency as
 * it was *around that moment* rather than a cumulative average that flattens
 * out and stops moving after the first few thousand events.
 */
class RollingLatency {
    public:
        void add(std::uint64_t nanos) {
            if (samples.size() == Window) samples.pop_front();
            samples.push_back(nanos);
        }
        /** Returns {p50, p99, p999}; zeros when no samples have been seen. */
        std::array<std::uint64_t, 3> percentiles() const {
            if (samples.empty()) return {0, 0, 0};
            std::vector<std::uint64_t> sorted(samples.begin(), samples.end());
            std::sort(sorted.begin(), sorted.end());
            const auto at = [&](double p) {
                return sorted[static_cast<std::size_t>(p * (sorted.size() - 1))];
            };
            return {at(0.50), at(0.99), at(0.999)};
        }
    private:
        static constexpr std::size_t Window = 1024;
        std::deque<std::uint64_t> samples;
};

/** @brief One execution, as the tape view needs it. */
struct TapeEntry {
    std::uint64_t tsNs;
    std::uint64_t priceRaw;
    std::uint64_t quantity;
    char aggressorSide; // 'B' or 'S'
};

void appendJsonLevels(std::string &out, const std::vector<lob::BookSnapshotLevel> &levels) {
    out.push_back('[');
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += '[' + std::to_string(levels[i].priceRaw) + ',' +
               std::to_string(levels[i].quantity) + ',' +
               std::to_string(levels[i].orderCount) + ']';
    }
    out.push_back(']');
}

} // namespace

int main(int argc, char **argv) {
    std::string input, output = "snapshots.jsonl", label;
    std::uint64_t snapshotEvery = 0; // 0 => auto-target ~1500 frames
    std::size_t depth = 12;
    std::uint64_t tick = 1;
    double headroomPct = 20.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (arg == "--input") input = next();
        else if (arg == "--output") output = next();
        else if (arg == "--label") label = next();
        else if (arg == "--every") snapshotEvery = std::strtoull(next().c_str(), nullptr, 10);
        else if (arg == "--depth") depth = std::strtoull(next().c_str(), nullptr, 10);
        else if (arg == "--tick") tick = std::strtoull(next().c_str(), nullptr, 10);
        else if (arg == "--headroom-pct") headroomPct = std::strtod(next().c_str(), nullptr);
        else {
            std::fprintf(stderr,
                "usage: snapshot_recorder --input FILE.csv [--output FILE.jsonl] [--label NAME]\n"
                "                        [--every N] [--depth N] [--tick N] [--headroom-pct N]\n");
            return 2;
        }
    }
    if (input.empty()) {
        std::fprintf(stderr, "snapshot_recorder: --input is required\n");
        return 2;
    }
    if (label.empty()) label = input;

    MappedFile file(input);
    std::vector<ParsedEvent> events;
    events.reserve(file.view().size() / 24);
    std::uint64_t minPriceRaw = UINT64_MAX, maxPriceRaw = 0;
    bool firstLine = true;
    forEachLine(file.view(), [&](std::string_view line) {
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
            if (e.kind == EventKind::New && e.type == OrderType::LIMIT) {
                if (price != 0) {
                    minPriceRaw = std::min(minPriceRaw, price);
                    maxPriceRaw = std::max(maxPriceRaw, price);
                }
            }
        }
        events.push_back(e);
    });

    if (minPriceRaw == UINT64_MAX) {
        std::fprintf(stderr, "snapshot_recorder: no priced LIMIT NEW rows found\n");
        return 1;
    }
    if (snapshotEvery == 0) {
        snapshotEvery = std::max<std::uint64_t>(1, events.size() / 1500);
    }

    const std::uint64_t span = maxPriceRaw - minPriceRaw;
    const std::uint64_t rawHeadroom =
        static_cast<std::uint64_t>(static_cast<double>(span) * headroomPct / 100.0) + tick;
    const std::uint64_t headroom = ((rawHeadroom + tick - 1) / tick) * tick;
    std::uint64_t ladderMin = minPriceRaw > headroom ? minPriceRaw - headroom : minPriceRaw % tick;
    if (ladderMin == 0) ladderMin = tick;
    const std::uint64_t ladderMax = maxPriceRaw + headroom;
    const std::size_t levelCount = static_cast<std::size_t>((ladderMax - ladderMin) / tick) + 1;

    OrderBook book(Price{ladderMin}, Price{tick}, levelCount);
    book.reserveOrderCapacity(events.size());
    MatchingEngine engine{book};

    std::FILE *out = std::fopen(output.c_str(), "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "snapshot_recorder: cannot write %s\n", output.c_str());
        return 1;
    }

    // Header line: everything the viewer needs to set up axes before it has
    // seen any frame.
    {
        std::string header = "{\"type\":\"meta\",\"label\":\"";
        for (char c : label) { if (c == '"' || c == '\\') header.push_back('\\'); header.push_back(c); }
        header += "\",\"events\":" + std::to_string(events.size()) +
                  ",\"tick\":" + std::to_string(tick) +
                  ",\"depth\":" + std::to_string(depth) +
                  ",\"every\":" + std::to_string(snapshotEvery) +
                  ",\"minPrice\":" + std::to_string(minPriceRaw) +
                  ",\"maxPrice\":" + std::to_string(maxPriceRaw) + "}\n";
        std::fwrite(header.data(), 1, header.size(), out);
    }

    BookSnapshot snapshot;
    std::vector<Execution> executionBuffer;
    std::unordered_map<std::string, RollingLatency> latency;
    std::vector<TapeEntry> tape;
    std::string line;
    line.reserve(1 << 14);

    std::uint64_t newCount = 0, cancelCount = 0, reduceCount = 0, tradeCount = 0;
    std::uint64_t buyQty = 0, sellQty = 0, errorCount = 0, frames = 0;

    for (std::size_t i = 0; i < events.size(); ++i) {
        const ParsedEvent &e = events[i];
        const Timestamp timestamp{e.tsNs};
        const OrderId orderId{e.orderId};

        try {
            const auto start = std::chrono::steady_clock::now();
            switch (e.kind) {
                case EventKind::New:
                    engine.processEvent(OrderEvent{lob::NewOrder{
                        orderId, Price{e.priceRaw}, Quantity{e.quantity}, timestamp, e.side, e.type}},
                        executionBuffer);
                    break;
                case EventKind::Cancel:
                    engine.processEvent(OrderEvent{lob::CancelOrder{orderId}}, executionBuffer);
                    break;
                case EventKind::Reduce:
                    engine.processEvent(OrderEvent{lob::ReduceOrder{orderId, Quantity{e.quantity}}},
                                        executionBuffer);
                    break;
            }
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());

            // Everything below this point is outside the timed region.
            const char *label_ = "CANCEL";
            if (e.kind == EventKind::New) {
                label_ = (e.type == OrderType::MARKET || !executionBuffer.empty()) ? "NEW_CROSS" : "NEW_REST";
                ++newCount;
            } else if (e.kind == EventKind::Reduce) {
                label_ = "REDUCE";
                ++reduceCount;
            } else {
                ++cancelCount;
            }
            latency[label_].add(elapsed);

            for (const auto &execution : executionBuffer) {
                const std::uint64_t qty = execution.getExecutionQuantity().getQuantity();
                ++tradeCount;
                if (e.side == OrderSide::BUY) buyQty += qty; else sellQty += qty;
                tape.push_back(TapeEntry{e.tsNs, execution.getExecutionPrice().getPrice(), qty,
                                         e.side == OrderSide::BUY ? 'B' : 'S'});
            }
        } catch (const std::exception &) {
            ++errorCount;
        }

        if ((i + 1) % snapshotEvery != 0 && i + 1 != events.size()) {
            continue;
        }

        book.captureSnapshot(snapshot, depth);
        snapshot.sequence = i + 1;
        snapshot.tsNs = e.tsNs;

        line.clear();
        line += "{\"seq\":" + std::to_string(snapshot.sequence);
        line += ",\"ts\":" + std::to_string(snapshot.tsNs);
        line += ",\"bb\":" + std::to_string(snapshot.bestBidRaw);
        line += ",\"ba\":" + std::to_string(snapshot.bestAskRaw);
        line += ",\"nb\":" + std::to_string(snapshot.bidLevelCount);
        line += ",\"na\":" + std::to_string(snapshot.askLevelCount);
        line += ",\"bids\":"; appendJsonLevels(line, snapshot.bids);
        line += ",\"asks\":"; appendJsonLevels(line, snapshot.asks);
        line += ",\"flow\":{\"new\":" + std::to_string(newCount) +
                ",\"cancel\":" + std::to_string(cancelCount) +
                ",\"reduce\":" + std::to_string(reduceCount) +
                ",\"trades\":" + std::to_string(tradeCount) +
                ",\"buyQty\":" + std::to_string(buyQty) +
                ",\"sellQty\":" + std::to_string(sellQty) + "}";

        line += ",\"lat\":{";
        bool firstLatency = true;
        for (const char *name : {"NEW_REST", "NEW_CROSS", "CANCEL", "REDUCE"}) {
            const auto it = latency.find(name);
            if (it == latency.end()) continue;
            const auto p = it->second.percentiles();
            if (!firstLatency) line.push_back(',');
            firstLatency = false;
            line += std::string("\"") + name + "\":[" + std::to_string(p[0]) + ',' +
                    std::to_string(p[1]) + ',' + std::to_string(p[2]) + ']';
        }
        line += "}";

        // Cap the tape carried in any one frame; a burst can produce far more
        // executions than a viewer can meaningfully display in one step.
        line += ",\"tape\":[";
        const std::size_t tapeStart = tape.size() > 24 ? tape.size() - 24 : 0;
        for (std::size_t t = tapeStart; t < tape.size(); ++t) {
            if (t != tapeStart) line.push_back(',');
            line += '[' + std::to_string(tape[t].tsNs) + ',' + std::to_string(tape[t].priceRaw) +
                    ',' + std::to_string(tape[t].quantity) + ",\"" + tape[t].aggressorSide + "\"]";
        }
        line += "]}\n";
        tape.clear();

        std::fwrite(line.data(), 1, line.size(), out);
        ++frames;
    }

    std::fclose(out);
    std::fprintf(stderr,
        "snapshot_recorder: %s\n"
        "  events replayed %zu (%llu errors)\n"
        "  frames written  %llu (every %llu events, depth %zu)\n"
        "  ladder          %zu levels, tick %llu\n"
        "  output          %s\n",
        input.c_str(), events.size(), static_cast<unsigned long long>(errorCount),
        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(snapshotEvery),
        depth, levelCount, static_cast<unsigned long long>(tick), output.c_str());
    return 0;
}
