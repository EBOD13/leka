// tools/itch/itch_to_csv.cpp

/**
 * @file
 * @brief Decodes a Nasdaq TotalView-ITCH 5.0 binary session file into CSV.
 *
 * The output is a faithful, lossless transcription of the ITCH trading
 * messages: one CSV row per ITCH message, with the original field names and
 * no interpretation applied. Mapping these rows onto lob::OrderEvent is a
 * separate concern and is deliberately not done here.
 *
 * ITCH framing is a 2-byte big-endian payload length followed by the payload.
 * Every payload shares a common header: a message-type byte, a 2-byte stock
 * locate code, a 2-byte tracking number, and a 6-byte timestamp holding
 * nanoseconds since midnight in US/Eastern on the session date.
 *
 * Messages that reference an order rather than a symbol (E, C, X, D, U, B)
 * carry no stock field. The symbol is recovered through the stock locate code
 * using the directory built from the 'R' messages at the head of the file.
 */

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/** @brief Reads a 16-bit big-endian field. */
inline std::uint16_t rd16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0]) << 8 | p[1];
}

/** @brief Reads a 32-bit big-endian field. */
inline std::uint32_t rd32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) << 24 |
           static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 |
           static_cast<std::uint32_t>(p[3]);
}

/** @brief Reads a 48-bit big-endian field, as used by ITCH timestamps. */
inline std::uint64_t rd48(const std::uint8_t *p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) {
        v = v << 8 | p[i];
    }
    return v;
}

/** @brief Reads a 64-bit big-endian field. */
inline std::uint64_t rd64(const std::uint8_t *p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = v << 8 | p[i];
    }
    return v;
}

/** @brief Returns an 8-byte ITCH stock field with its padding spaces removed. */
inline std::string_view symbolAt(const std::uint8_t *p) {
    std::string_view s(reinterpret_cast<const char *>(p), 8);
    const auto end = s.find_last_not_of(' ');
    return end == std::string_view::npos ? std::string_view{} : s.substr(0, end + 1);
}

/** @brief Human-readable name for an ITCH message type byte. */
const char *messageName(std::uint8_t type) {
    switch (type) {
        case 'A': return "ADD_ORDER";
        case 'F': return "ADD_ORDER_MPID";
        case 'E': return "EXECUTED";
        case 'C': return "EXECUTED_WITH_PRICE";
        case 'X': return "CANCEL_PARTIAL";
        case 'D': return "DELETE";
        case 'U': return "REPLACE";
        case 'P': return "TRADE_NON_CROSS";
        case 'Q': return "TRADE_CROSS";
        case 'B': return "TRADE_BROKEN";
        case 'S': return "SYSTEM_EVENT";
        case 'H': return "TRADING_ACTION";
        default:  return "OTHER";
    }
}

/** @brief Appends an unsigned integer without going through stdio formatting. */
void appendUInt(std::string &out, std::uint64_t v) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, res.ptr);
}

/**
 * @brief Appends an ITCH price as a fixed-point decimal.
 *
 * ITCH prices are unsigned integers with four implied decimal places, so the
 * conversion is exact and no floating point is involved.
 */
void appendPrice(std::string &out, std::uint32_t raw) {
    appendUInt(out, raw / 10000);
    out.push_back('.');
    const std::uint32_t frac = raw % 10000;
    out.push_back(static_cast<char>('0' + frac / 1000 % 10));
    out.push_back(static_cast<char>('0' + frac / 100 % 10));
    out.push_back(static_cast<char>('0' + frac / 10 % 10));
    out.push_back(static_cast<char>('0' + frac % 10));
}

/** @brief Appends a zero-padded integer of a fixed width. */
void appendPadded(std::string &out, std::uint64_t v, int width) {
    char buf[24];
    for (int i = width - 1; i >= 0; --i) {
        buf[i] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    out.append(buf, static_cast<std::size_t>(width));
}

/** @brief Appends an ITCH timestamp as HH:MM:SS.nnnnnnnnn in exchange time. */
void appendClock(std::string &out, std::uint64_t nanosSinceMidnight) {
    const std::uint64_t nanos = nanosSinceMidnight % 1000000000ULL;
    const std::uint64_t seconds = nanosSinceMidnight / 1000000000ULL;
    appendPadded(out, seconds / 3600, 2);
    out.push_back(':');
    appendPadded(out, seconds / 60 % 60, 2);
    out.push_back(':');
    appendPadded(out, seconds % 60, 2);
    out.push_back('.');
    appendPadded(out, nanos, 9);
}

/** @brief Command line configuration for a single conversion run. */
struct Options {
    /** Path to the decompressed ITCH session file. */
    std::string input;
    /** Path of the CSV to write, or "-" for standard output. */
    std::string output = "-";
    /** Symbols to keep; an empty set keeps every symbol. */
    std::vector<std::string> symbols;
    /** Session date as YYYY-MM-DD, used to derive epoch timestamps. */
    std::string date;
    /** Stop after this many emitted rows; zero means no limit. */
    std::uint64_t limit = 0;
    /** Whether to emit the S and H session-control messages. */
    bool system = true;
};

/** @brief Prints the usage banner. */
void usage() {
    std::fprintf(stderr,
        "usage: itch_to_csv --input FILE [options]\n"
        "\n"
        "  --input FILE      decompressed ITCH 5.0 session file (required)\n"
        "  --output FILE     CSV destination, '-' for stdout (default '-')\n"
        "  --symbol SYM      keep only this symbol; repeatable (default: all)\n"
        "  --date YYYY-MM-DD session date; inferred from the filename if omitted\n"
        "  --limit N         stop after N rows\n"
        "  --no-system       omit SYSTEM_EVENT and TRADING_ACTION rows\n");
}

/**
 * @brief Derives the Unix epoch nanoseconds of midnight on the session date.
 *
 * ITCH timestamps are offsets from midnight in US/Eastern, so the base is
 * resolved through the tz database rather than a fixed UTC offset. That keeps
 * EST and EDT sessions correct without a hand-maintained DST table.
 */
std::uint64_t epochBaseNanos(const std::string &date) {
    if (date.size() != 10) {
        return 0;
    }
    std::tm tm{};
    tm.tm_year = std::stoi(date.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(date.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(date.substr(8, 2));
    tm.tm_isdst = -1;

    const char *previous = std::getenv("TZ");
    const std::string saved = previous != nullptr ? previous : "";
    setenv("TZ", "America/New_York", 1);
    tzset();
    const std::time_t midnight = std::mktime(&tm);
    if (previous != nullptr) {
        setenv("TZ", saved.c_str(), 1);
    } else {
        unsetenv("TZ");
    }
    tzset();

    return midnight == static_cast<std::time_t>(-1)
        ? 0
        : static_cast<std::uint64_t>(midnight) * 1000000000ULL;
}

/** @brief Recovers a YYYY-MM-DD date from a leading YYYYMMDD in the filename. */
std::string dateFromPath(const std::string &path) {
    const auto slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.size() < 8) {
        return {};
    }
    const std::string digits = name.substr(0, 8);
    if (!std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return {};
    }
    return digits.substr(0, 4) + "-" + digits.substr(4, 2) + "-" + digits.substr(6, 2);
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
        } else if (arg == "--output") {
            options.output = next();
        } else if (arg == "--symbol") {
            options.symbols.push_back(next());
        } else if (arg == "--date") {
            options.date = next();
        } else if (arg == "--limit") {
            options.limit = std::strtoull(next().c_str(), nullptr, 10);
        } else if (arg == "--no-system") {
            options.system = false;
        } else {
            usage();
            return 2;
        }
    }
    if (options.input.empty()) {
        usage();
        return 2;
    }
    if (options.date.empty()) {
        options.date = dateFromPath(options.input);
    }
    const std::uint64_t epochBase = epochBaseNanos(options.date);

    const int fd = ::open(options.input.c_str(), O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "itch_to_csv: cannot open %s: %s\n",
                     options.input.c_str(), std::strerror(errno));
        return 1;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        std::fprintf(stderr, "itch_to_csv: cannot stat input\n");
        ::close(fd);
        return 1;
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    auto *base = static_cast<const std::uint8_t *>(
        ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (base == MAP_FAILED) {
        std::fprintf(stderr, "itch_to_csv: mmap failed: %s\n", std::strerror(errno));
        ::close(fd);
        return 1;
    }
    ::madvise(const_cast<std::uint8_t *>(base), size, MADV_SEQUENTIAL);

    std::FILE *out = options.output == "-"
        ? stdout
        : std::fopen(options.output.c_str(), "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "itch_to_csv: cannot write %s: %s\n",
                     options.output.c_str(), std::strerror(errno));
        ::munmap(const_cast<std::uint8_t *>(base), size);
        ::close(fd);
        return 1;
    }

    // Symbols are matched on the locate code, so the filter resolves to a set
    // of codes once the directory messages have been seen.
    std::vector<std::string> wanted = options.symbols;
    const bool filtering = !wanted.empty();
    std::vector<bool> keepLocate;
    std::unordered_map<std::uint16_t, std::string> locateToSymbol;

    std::string buffer;
    buffer.reserve(1 << 22);
    buffer.append("seq,ts_ns,epoch_ns,clock_et,msg,name,symbol,order_ref,side,"
                  "shares,price,new_order_ref,match_number,printable,attribution\n");

    std::uint64_t messages = 0;
    std::uint64_t rows = 0;
    std::size_t offset = 0;
    bool truncated = false;

    while (offset + 2 <= size) {
        const std::uint16_t length = rd16(base + offset);
        if (length == 0 || offset + 2 + length > size) {
            truncated = offset + 2 + length > size && length != 0;
            break;
        }
        const std::uint8_t *m = base + offset + 2;
        offset += 2 + length;
        ++messages;

        const std::uint8_t type = m[0];
        const std::uint16_t locate = rd16(m + 1);

        // The directory precedes the trading messages and defines the filter.
        if (type == 'R') {
            const std::string symbol(symbolAt(m + 11));
            locateToSymbol.emplace(locate, symbol);
            if (filtering) {
                if (keepLocate.size() <= locate) {
                    keepLocate.resize(locate + 1, false);
                }
                keepLocate[locate] = std::find(wanted.begin(), wanted.end(), symbol) != wanted.end();
            }
            continue;
        }

        const bool isSystem = type == 'S' || type == 'H';
        if (isSystem && !options.system) {
            continue;
        }
        // System events carry locate 0 and describe the whole session, so they
        // survive symbol filtering.
        if (filtering && type != 'S') {
            if (locate >= keepLocate.size() || !keepLocate[locate]) {
                continue;
            }
        }

        const char *name = messageName(type);
        if (std::strcmp(name, "OTHER") == 0) {
            continue;
        }

        const std::uint64_t nanos = rd48(m + 5);

        appendUInt(buffer, ++rows);
        buffer.push_back(',');
        appendUInt(buffer, nanos);
        buffer.push_back(',');
        if (epochBase != 0) {
            appendUInt(buffer, epochBase + nanos);
        }
        buffer.push_back(',');
        appendClock(buffer, nanos);
        buffer.push_back(',');
        buffer.push_back(static_cast<char>(type));
        buffer.push_back(',');
        buffer.append(name);
        buffer.push_back(',');

        // symbol
        if (type == 'A' || type == 'F' || type == 'P') {
            buffer.append(symbolAt(m + 24));
        } else if (type == 'Q') {
            buffer.append(symbolAt(m + 19));
        } else if (type == 'H') {
            buffer.append(symbolAt(m + 11));
        } else if (type != 'S') {
            const auto it = locateToSymbol.find(locate);
            if (it != locateToSymbol.end()) {
                buffer.append(it->second);
            }
        }
        buffer.push_back(',');

        // order_ref
        if (type == 'A' || type == 'F' || type == 'E' || type == 'C' ||
            type == 'X' || type == 'D' || type == 'U' || type == 'P') {
            appendUInt(buffer, rd64(m + 11));
        }
        buffer.push_back(',');

        // side
        if (type == 'A' || type == 'F' || type == 'P') {
            buffer.push_back(static_cast<char>(m[19]));
        }
        buffer.push_back(',');

        // shares
        if (type == 'A' || type == 'F' || type == 'P') {
            appendUInt(buffer, rd32(m + 20));
        } else if (type == 'E' || type == 'C' || type == 'X') {
            appendUInt(buffer, rd32(m + 19));
        } else if (type == 'U') {
            appendUInt(buffer, rd32(m + 27));
        } else if (type == 'Q') {
            appendUInt(buffer, rd64(m + 11));
        }
        buffer.push_back(',');

        // price
        if (type == 'A' || type == 'F' || type == 'P') {
            appendPrice(buffer, rd32(m + 32));
        } else if (type == 'C') {
            appendPrice(buffer, rd32(m + 32));
        } else if (type == 'U') {
            appendPrice(buffer, rd32(m + 31));
        } else if (type == 'Q') {
            appendPrice(buffer, rd32(m + 27));
        }
        buffer.push_back(',');

        // new_order_ref
        if (type == 'U') {
            appendUInt(buffer, rd64(m + 19));
        }
        buffer.push_back(',');

        // match_number
        if (type == 'E' || type == 'C') {
            appendUInt(buffer, rd64(m + 23));
        } else if (type == 'P') {
            appendUInt(buffer, rd64(m + 36));
        } else if (type == 'Q') {
            appendUInt(buffer, rd64(m + 31));
        } else if (type == 'B') {
            appendUInt(buffer, rd64(m + 11));
        }
        buffer.push_back(',');

        // printable, then attribution. The trailing column doubles as the
        // event code for S and the trading state for H.
        if (type == 'C') {
            buffer.push_back(static_cast<char>(m[31]));
        }
        buffer.push_back(',');
        if (type == 'F') {
            std::string_view mpid(reinterpret_cast<const char *>(m + 36), 4);
            const auto end = mpid.find_last_not_of(' ');
            if (end != std::string_view::npos) {
                buffer.append(mpid.substr(0, end + 1));
            }
        } else if (type == 'S') {
            buffer.push_back(static_cast<char>(m[11]));
        } else if (type == 'H') {
            buffer.push_back(static_cast<char>(m[19]));
        }
        buffer.push_back('\n');

        if (buffer.size() >= (1 << 21)) {
            std::fwrite(buffer.data(), 1, buffer.size(), out);
            buffer.clear();
        }
        if (options.limit != 0 && rows >= options.limit) {
            break;
        }
    }

    std::fwrite(buffer.data(), 1, buffer.size(), out);
    if (out != stdout) {
        std::fclose(out);
    }
    ::munmap(const_cast<std::uint8_t *>(base), size);
    ::close(fd);

    std::fprintf(stderr,
                 "itch_to_csv: %s (%.2f GB)\n"
                 "  session date   %s%s\n"
                 "  symbols known  %zu\n"
                 "  messages read  %llu\n"
                 "  rows written   %llu\n%s",
                 options.input.c_str(), static_cast<double>(size) / 1e9,
                 options.date.empty() ? "unknown" : options.date.c_str(),
                 epochBase == 0 ? " (epoch_ns left blank)" : "",
                 locateToSymbol.size(),
                 static_cast<unsigned long long>(messages),
                 static_cast<unsigned long long>(rows),
                 truncated ? "  warning: input ends mid-message\n" : "");
    return 0;
}
