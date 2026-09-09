#include "note_format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
using psychgpu::NoteData;

namespace {

constexpr std::size_t DEFAULT_CHUNK_RECORDS = 1'000'000;
constexpr std::size_t DEFAULT_MERGE_FAN_IN = 64;
constexpr std::size_t INPUT_BUFFER_BYTES = 1U << 20;
constexpr double DEFAULT_BPM = 100.0;
constexpr double DEFAULT_SECTION_BEATS = 4.0;
constexpr std::uint64_t MAX_SAFE_JSON_INTEGER = 9'007'199'254'740'991ULL;

struct Error final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Json final {
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Value value{nullptr};

    Json() = default;
    Json(std::nullptr_t) : value(nullptr) {}
    Json(bool v) : value(v) {}
    Json(double v) : value(v) {}
    Json(std::string v) : value(std::move(v)) {}
    Json(Array v) : value(std::move(v)) {}
    Json(Object v) : value(std::move(v)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(value); }
    bool isBool() const { return std::holds_alternative<bool>(value); }
    bool isNumber() const { return std::holds_alternative<double>(value); }
    bool isString() const { return std::holds_alternative<std::string>(value); }
    bool isArray() const { return std::holds_alternative<Array>(value); }
    bool isObject() const { return std::holds_alternative<Object>(value); }
    bool boolean(bool fallback = false) const {
        return isBool() ? std::get<bool>(value) : fallback;
    }
    double number(double fallback = 0.0) const {
        return isNumber() ? std::get<double>(value) : fallback;
    }
    const std::string& string() const {
        static const std::string empty;
        return isString() ? std::get<std::string>(value) : empty;
    }
    const Array& array() const {
        static const Array empty;
        return isArray() ? std::get<Array>(value) : empty;
    }
    const Object& object() const {
        static const Object empty;
        return isObject() ? std::get<Object>(value) : empty;
    }
};

class JsonReader final {
public:
    explicit JsonReader(const fs::path& path) : in_(path, std::ios::binary), path_(path) {
        if (!in_) throw Error("cannot open " + path.string());
        buffer_.resize(INPUT_BUFFER_BYTES);
    }

    int peek() {
        if (!held_) {
            current_ = readByte();
            held_ = true;
        }
        return current_;
    }

    int get() {
        const int c = peek();
        held_ = false;
        if (c == '\n') { ++line_; column_ = 0; }
        else if (c >= 0) { ++column_; }
        return c;
    }

    void whitespace() {
        while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') get();
    }

    int next() { whitespace(); return peek(); }

    [[noreturn]] void fail(const std::string& text) const {
        throw Error(path_.string() + ":" + std::to_string(line_) + ":" +
                    std::to_string(column_) + ": " + text);
    }

    void expect(char wanted) {
        whitespace();
        if (get() != wanted) fail(std::string("expected '") + wanted + "'");
    }

    bool consume(char wanted) {
        whitespace();
        if (peek() != wanted) return false;
        get();
        return true;
    }

    std::string string() {
        whitespace();
        if (get() != '"') fail("expected string");
        std::string out;
        for (;;) {
            const int raw = get();
            if (raw < 0) fail("EOF in string");
            const auto c = static_cast<unsigned char>(raw);
            if (c == '"') return out;
            if (c < 0x20) fail("control character in string");
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            switch (get()) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = hex4();
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (get() != '\\' || get() != 'u') fail("unpaired high surrogate");
                        const std::uint32_t low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) fail("invalid low surrogate");
                        cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                        fail("unpaired low surrogate");
                    }
                    utf8(out, cp);
                    break;
                }
                default: fail("invalid escape");
            }
        }
    }

    double number() {
        whitespace();
        std::string token;
        if (peek() == '-') token.push_back(static_cast<char>(get()));
        if (peek() == '0') token.push_back(static_cast<char>(get()));
        else {
            if (peek() < '1' || peek() > '9') fail("invalid number");
            while (peek() >= '0' && peek() <= '9') token.push_back(static_cast<char>(get()));
        }
        if (peek() == '.') {
            token.push_back(static_cast<char>(get()));
            if (peek() < '0' || peek() > '9') fail("invalid fraction");
            while (peek() >= '0' && peek() <= '9') token.push_back(static_cast<char>(get()));
        }
        if (peek() == 'e' || peek() == 'E') {
            token.push_back(static_cast<char>(get()));
            if (peek() == '+' || peek() == '-') token.push_back(static_cast<char>(get()));
            if (peek() < '0' || peek() > '9') fail("invalid exponent");
            while (peek() >= '0' && peek() <= '9') token.push_back(static_cast<char>(get()));
        }
        char* end = nullptr;
        const double result = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || !std::isfinite(result))
            fail("number is outside the finite double range");
        return result;
    }

    Json value() {
        whitespace();
        switch (peek()) {
            case 'n': keyword("null"); return Json(nullptr);
            case 't': keyword("true"); return Json(true);
            case 'f': keyword("false"); return Json(false);
            case '"': return Json(string());
            case '[': return Json(array());
            case '{': return Json(object());
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return Json(number());
                fail("expected value");
        }
    }

    void skipValue() {
        whitespace();
        if (peek() == '"') { string(); return; }
        if (peek() == '-' || (peek() >= '0' && peek() <= '9')) { number(); return; }
        if (peek() == 'n') { keyword("null"); return; }
        if (peek() == 't') { keyword("true"); return; }
        if (peek() == 'f') { keyword("false"); return; }
        if (consume('[')) {
            if (consume(']')) return;
            for (;;) {
                skipValue();
                if (consume(']')) return;
                expect(',');
            }
        }
        if (consume('{')) {
            if (consume('}')) return;
            for (;;) {
                string(); expect(':'); skipValue();
                if (consume('}')) return;
                expect(',');
            }
        }
        fail("expected value to skip");
    }

private:
    std::ifstream in_;
    fs::path path_;
    std::vector<char> buffer_;
    std::size_t pos_ = 0, size_ = 0;
    int current_ = -1;
    bool held_ = false;
    std::uint64_t line_ = 1, column_ = 0;

    int readByte() {
        if (pos_ == size_) {
            in_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
            size_ = static_cast<std::size_t>(in_.gcount());
            pos_ = 0;
            if (size_ == 0) return -1;
        }
        return static_cast<unsigned char>(buffer_[pos_++]);
    }

    void keyword(std::string_view word) {
        for (char c : word) if (get() != c) fail("invalid keyword");
    }

    std::uint32_t hex4() {
        std::uint32_t result = 0;
        for (int i = 0; i < 4; ++i) {
            result <<= 4;
            const int c = get();
            if (c >= '0' && c <= '9') result |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') result |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') result |= static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return result;
    }

    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    Json::Array array() {
        expect('[');
        Json::Array result;
        if (consume(']')) return result;
        for (;;) {
            result.push_back(value());
            if (consume(']')) return result;
            expect(',');
        }
    }

    Json::Object object() {
        expect('{');
        Json::Object result;
        if (consume('}')) return result;
        for (;;) {
            std::string key = string();
            expect(':');
            result.insert_or_assign(std::move(key), value());
            if (consume('}')) return result;
            expect(',');
        }
    }
};

void writeEscaped(std::ostream& out, const std::string& text) {
    out.put('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned>(c) << std::dec << std::setfill(' ');
                } else out.put(static_cast<char>(c));
        }
    }
    out.put('"');
}

void writeJson(std::ostream& out, const Json& json, int depth = 0) {
    const auto indent = [&](int amount) { for (int i = 0; i < amount; ++i) out.put(' '); };
    if (json.isNull()) out << "null";
    else if (json.isBool()) out << (json.boolean() ? "true" : "false");
    else if (json.isNumber()) out << std::setprecision(17) << json.number();
    else if (json.isString()) writeEscaped(out, json.string());
    else if (json.isArray()) {
        out.put('[');
        const auto& a = json.array();
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i) out.put(',');
            out.put('\n'); indent(depth + 2); writeJson(out, a[i], depth + 2);
        }
        if (!a.empty()) { out.put('\n'); indent(depth); }
        out.put(']');
    } else {
        out.put('{');
        std::size_t i = 0;
        for (const auto& [key, value] : json.object()) {
            if (i++) out.put(',');
            out.put('\n'); indent(depth + 2); writeEscaped(out, key); out << ": ";
            writeJson(out, value, depth + 2);
        }
        if (!json.object().empty()) { out.put('\n'); indent(depth); }
        out.put('}');
    }
}

void writeU16(std::ostream& out, std::uint16_t v) {
    out.put(static_cast<char>(v)); out.put(static_cast<char>(v >> 8));
}
void writeU64(std::ostream& out, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) out.put(static_cast<char>(v >> (i * 8)));
}
bool readU16(std::istream& in, std::uint16_t& v) {
    const int a = in.get();
    if (a < 0) return false;
    const int b = in.get();
    if (b < 0) throw Error("truncated temporary chunk");
    v = static_cast<std::uint16_t>(a | (b << 8)); return true;
}
bool readU64(std::istream& in, std::uint64_t& v) {
    int first = in.get();
    if (first < 0) return false;
    v = static_cast<unsigned char>(first);
    for (unsigned i = 1; i < 8; ++i) {
        const int c = in.get();
        if (c < 0) throw Error("truncated temporary chunk");
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(c)) << (i * 8);
    }
    return true;
}

struct SortRecord final { NoteData note{}; std::uint64_t sequence = 0; };
struct SortLess final {
    bool operator()(const SortRecord& a, const SortRecord& b) const {
        if (a.note.timestamp_ms != b.note.timestamp_ms)
            return a.note.timestamp_ms < b.note.timestamp_ms;
        const auto al = psychgpu::laneIndex(a.note.lane_id);
        const auto bl = psychgpu::laneIndex(b.note.lane_id);
        if (al != bl) return al < bl;
        return a.sequence < b.sequence;
    }
};
void writeSort(std::ostream& out, const SortRecord& r) {
    writeU64(out, r.note.timestamp_ms); writeU16(out, r.note.sustain_ms);
    out.put(static_cast<char>(r.note.lane_id)); out.put(static_cast<char>(r.note.note_type));
    writeU64(out, r.sequence);
}
bool readSort(std::istream& in, SortRecord& r) {
    if (!readU64(in, r.note.timestamp_ms)) return false;
    if (!readU16(in, r.note.sustain_ms)) throw Error("truncated temporary chunk");
    const int lane = in.get(), type = in.get();
    if (lane < 0 || type < 0) throw Error("truncated temporary chunk");
    r.note.lane_id = static_cast<std::uint8_t>(lane);
    r.note.note_type = static_cast<std::uint8_t>(type);
    if (!readU64(in, r.sequence)) throw Error("truncated temporary chunk");
    return true;
}
void writeNote(std::ostream& out, const NoteData& n) {
    writeU64(out, n.timestamp_ms); writeU16(out, n.sustain_ms);
    out.put(static_cast<char>(n.lane_id)); out.put(static_cast<char>(n.note_type));
}

class ExternalSorter final {
public:
    ExternalSorter(fs::path directory, std::size_t chunkRecords,
                   std::size_t fanIn, bool keep)
        : directory_(std::move(directory)), chunkRecords_(chunkRecords),
          fanIn_(fanIn), keep_(keep) {
        if (!chunkRecords_) throw Error("chunk-records must be positive");
        if (fanIn_ < 2) throw Error("merge-fan-in must be at least 2");
        fs::create_directories(directory_);
        records_.reserve(chunkRecords_);
    }
    ~ExternalSorter() { if (!keep_) cleanup(); }

    void push(const SortRecord& r) {
        records_.push_back(r);
        if (records_.size() == chunkRecords_) flush();
    }

    std::uint64_t finish(const fs::path& destination) {
        flush();
        std::size_t pass = 0;
        while (chunks_.size() > fanIn_) {
            std::vector<fs::path> next;
            for (std::size_t begin = 0; begin < chunks_.size(); begin += fanIn_) {
                const std::size_t end = std::min(chunks_.size(), begin + fanIn_);
                fs::path target = directory_ / ("merge-" + std::to_string(pass) + "-" +
                                                  std::to_string(next.size()) + ".tmp");
                mergeGroup(chunks_, begin, end, target, false);
                next.push_back(target); allFiles_.push_back(target);
            }
            if (!keep_) for (const auto& p : chunks_) { std::error_code ec; fs::remove(p, ec); }
            chunks_ = std::move(next); ++pass;
        }
        fs::create_directories(destination.parent_path());
        const fs::path partial = destination.string() + ".partial";
        const std::uint64_t count = mergeGroup(chunks_, 0, chunks_.size(), partial, true);
        replace(partial, destination);
        return count;
    }

private:
    fs::path directory_;
    std::size_t chunkRecords_, fanIn_;
    bool keep_;
    std::vector<SortRecord> records_;
    std::vector<fs::path> chunks_, allFiles_;
    std::size_t serial_ = 0;

    void flush() {
        if (records_.empty()) return;
        std::sort(records_.begin(), records_.end(), SortLess{});
        fs::path path = directory_ / ("chunk-" + std::to_string(serial_++) + ".tmp");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("cannot create " + path.string());
        for (const auto& r : records_) writeSort(out, r);
        out.flush(); if (!out) throw Error("failed writing " + path.string());
        chunks_.push_back(path); allFiles_.push_back(path); records_.clear();
    }

    static std::uint64_t mergeGroup(const std::vector<fs::path>& paths,
                                    std::size_t begin, std::size_t end,
                                    const fs::path& target, bool notesOnly) {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("cannot create " + target.string());
        if (begin == end) { out.close(); return 0; }
        struct Cursor { std::ifstream in; SortRecord record; };
        struct Item { SortRecord record; std::size_t cursor; };
        struct Greater { bool operator()(const Item& a, const Item& b) const {
            return SortLess{}(b.record, a.record);
        }};
        std::vector<Cursor> cursors;
        std::priority_queue<Item, std::vector<Item>, Greater> heap;
        cursors.reserve(end - begin);
        for (std::size_t i = begin; i < end; ++i) {
            Cursor c; c.in.open(paths[i], std::ios::binary);
            if (!c.in) throw Error("cannot reopen " + paths[i].string());
            if (readSort(c.in, c.record)) {
                cursors.push_back(std::move(c));
                heap.push({cursors.back().record, cursors.size() - 1});
            }
        }
        std::uint64_t count = 0;
        while (!heap.empty()) {
            const Item item = heap.top(); heap.pop();
            if (notesOnly) writeNote(out, item.record.note); else writeSort(out, item.record);
            ++count;
            auto& cursor = cursors[item.cursor];
            if (readSort(cursor.in, cursor.record)) heap.push({cursor.record, item.cursor});
        }
        out.flush(); if (!out) throw Error("failed writing " + target.string());
        return count;
    }

    static void replace(const fs::path& from, const fs::path& to) {
        std::error_code ec; fs::remove(to, ec); ec.clear(); fs::rename(from, to, ec);
        if (ec) throw Error("cannot install " + to.string() + ": " + ec.message());
    }
    void cleanup() noexcept {
        for (const auto& p : allFiles_) { std::error_code ec; fs::remove(p, ec); }
    }
};

struct Event final {
    std::uint64_t timestamp = 0, sequence = 0;
    std::string name;
    Json value1, value2;
};
struct Section final {
    bool mustHit = false, changeBpm = false;
    double bpm = DEFAULT_BPM, beats = DEFAULT_SECTION_BEATS;
};
struct Options final {
    fs::path input, output, temporary;
    std::optional<fs::path> events;
    std::size_t chunkRecords = DEFAULT_CHUNK_RECORDS;
    std::size_t mergeFanIn = DEFAULT_MERGE_FAN_IN;
    std::uint32_t lanesPerSide = 4;
    bool keepTemporary = false;
};

class Compiler final {
public:
    explicit Compiler(Options options)
        : options_(std::move(options)),
          sorter_(options_.temporary, options_.chunkRecords,
                  options_.mergeFanIn, options_.keepTemporary) {
        if (!options_.lanesPerSide || options_.lanesPerSide * 2 > 128)
            throw Error("lanes-per-side must be in 1..64");
    }

    void run() {
        scanMetadata(options_.input, true);
        initialBpm_ = metadata_.count("bpm") ? metadata_.at("bpm").number(DEFAULT_BPM) : DEFAULT_BPM;
        if (!std::isfinite(initialBpm_) || initialBpm_ <= 0) throw Error("song BPM must be positive");
        scanNotes(options_.input, false);
        if (options_.events) {
            scanMetadata(*options_.events, false);
            scanNotes(*options_.events, true);
        }
        std::sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) {
            return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.sequence < b.sequence;
        });
        noteCount_ = sorter_.finish(options_.output / "notes.bin");
        writeInfo(options_.output / "song_info.json");
        std::cout << "source notes: " << sourceNotes_ << "\n"
                  << "binary records: " << noteCount_ << "\n"
                  << "events: " << events_.size() << "\n"
                  << "split holds: " << splitHolds_ << "\n";
    }

private:
    Options options_;
    ExternalSorter sorter_;
    Json::Object metadata_;
    std::vector<Event> events_;
    std::unordered_map<std::string, std::uint8_t> typeIds_;
    std::vector<std::string> types_;
    double initialBpm_ = DEFAULT_BPM, currentBpm_ = DEFAULT_BPM, sectionStart_ = 0;
    std::uint64_t sequence_ = 0, eventSequence_ = 0, spoolSequence_ = 0;
    std::uint64_t sourceNotes_ = 0, noteCount_ = 0, splitHolds_ = 0;

    static std::uint64_t milliseconds(double value, bool duration = false) {
        if (!std::isfinite(value)) throw Error("non-finite timestamp or duration");
        if (duration && value <= 0) return 0;
        if (value < 0) throw Error("negative note timestamp");
        const long double rounded = std::round(static_cast<long double>(value));
        if (rounded > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            throw Error("millisecond value exceeds uint64");
        return static_cast<std::uint64_t>(rounded);
    }

    void scanMetadata(const fs::path& path, bool keepMetadata) {
        JsonReader r(path); r.expect('{');
        if (r.consume('}')) return;
        for (;;) {
            std::string key = r.string(); r.expect(':');
            if (key == "song" && r.next() == '{') scanSongMetadata(r, keepMetadata);
            else if (key == "events") parseEvents(r);
            else if (key == "notes") r.skipValue();
            else if (keepMetadata) metadata_.insert_or_assign(std::move(key), r.value());
            else r.skipValue();
            if (r.consume('}')) break; r.expect(',');
        }
        r.whitespace(); if (r.peek() >= 0) r.fail("trailing JSON data");
    }

    void scanSongMetadata(JsonReader& r, bool keepMetadata) {
        r.expect('{'); if (r.consume('}')) return;
        for (;;) {
            std::string key = r.string(); r.expect(':');
            if (key == "events") parseEvents(r);
            else if (key == "notes") r.skipValue();
            else if (keepMetadata) metadata_.insert_or_assign(std::move(key), r.value());
            else r.skipValue();
            if (r.consume('}')) return; r.expect(',');
        }
    }

    void scanNotes(const fs::path& path, bool eventsOnly) {
        currentBpm_ = initialBpm_; sectionStart_ = 0;
        JsonReader r(path); r.expect('{');
        if (r.consume('}')) return;
        for (;;) {
            std::string key = r.string(); r.expect(':');
            if (key == "song" && r.next() == '{') scanSongNotes(r, eventsOnly);
            else if (key == "notes") parseSections(r, eventsOnly);
            else r.skipValue();
            if (r.consume('}')) break; r.expect(',');
        }
    }

    void scanSongNotes(JsonReader& r, bool eventsOnly) {
        r.expect('{'); if (r.consume('}')) return;
        for (;;) {
            const std::string key = r.string(); r.expect(':');
            if (key == "notes") parseSections(r, eventsOnly); else r.skipValue();
            if (r.consume('}')) return; r.expect(',');
        }
    }

    void parseSections(JsonReader& r, bool eventsOnly) {
        r.expect('['); if (r.consume(']')) return;
        for (;;) {
            parseSection(r, eventsOnly);
            if (r.consume(']')) return; r.expect(',');
        }
    }

    void parseSection(JsonReader& r, bool eventsOnly) {
        Section section; section.bpm = currentBpm_;
        fs::create_directories(options_.temporary);
        const fs::path spool = options_.temporary / ("section-" + std::to_string(spoolSequence_++) + ".jsonl");
        std::ofstream out(spool, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("cannot create " + spool.string());
        r.expect('{');
        if (!r.consume('}')) for (;;) {
            const std::string key = r.string(); r.expect(':');
            if (key == "sectionNotes") spoolNotes(r, out);
            else {
                Json v = r.value();
                if (key == "mustHitSection") section.mustHit = v.boolean();
                else if (key == "changeBPM") section.changeBpm = v.boolean();
                else if (key == "bpm") section.bpm = v.number(currentBpm_);
                else if (key == "sectionBeats") section.beats = v.number(DEFAULT_SECTION_BEATS);
                else if (key == "lengthInSteps") section.beats = v.number(16) / 4.0;
            }
            if (r.consume('}')) break; r.expect(',');
        }
        out.close();
        if (section.changeBpm) {
            if (!std::isfinite(section.bpm) || section.bpm <= 0) throw Error("section BPM must be positive");
            currentBpm_ = section.bpm;
            if (!eventsOnly) events_.push_back({milliseconds(sectionStart_), eventSequence_++,
                                                "__bpm_change", Json(currentBpm_), Json(nullptr)});
        }
        replaySpool(spool, section, eventsOnly);
        if (!std::isfinite(section.beats) || section.beats < 0) throw Error("invalid section length");
        sectionStart_ += section.beats * 60000.0 / currentBpm_;
        if (!options_.keepTemporary) { std::error_code ec; fs::remove(spool, ec); }
    }

    static void spoolNotes(JsonReader& r, std::ostream& out) {
        r.expect('['); if (r.consume(']')) return;
        for (;;) {
            Json entry = r.value(); writeJson(out, entry); out.put('\n');
            if (r.consume(']')) return; r.expect(',');
        }
    }

    void replaySpool(const fs::path& spool, const Section& section, bool eventsOnly) {
        JsonReader r(spool);
        while (r.next() >= 0) processEntry(r.value(), section, eventsOnly);
    }

    void processEntry(const Json& entry, const Section& section, bool eventsOnly) {
        const auto& a = entry.array();
        if (a.size() < 2 || !a[0].isNumber() || !a[1].isNumber()) {
            std::cerr << "warning: malformed section entry skipped\n"; return;
        }
        const long long rawLane = std::llround(a[1].number());
        if (rawLane < 0) { legacyEvent(a); return; }
        if (eventsOnly) return;
        const double sustain = a.size() > 2 && a[2].isNumber() ? a[2].number() : 0;
        const std::uint8_t type = a.size() > 3 ? noteType(a[3]) : psychgpu::NOTE_TYPE_NORMAL;
        emit(milliseconds(a[0].number()), milliseconds(sustain, true), rawLane, section.mustHit, type);
        ++sourceNotes_;
    }

    void legacyEvent(const Json::Array& a) {
        if (a.size() < 3) { std::cerr << "warning: malformed legacy event skipped\n"; return; }
        events_.push_back({milliseconds(a[0].number()), eventSequence_++, scalar(a[2]),
                           a.size() > 3 ? a[3] : Json(std::string()),
                           a.size() > 4 ? a[4] : Json(std::string())});
    }

    void parseEvents(JsonReader& r) {
        Json root = r.value();
        for (const Json& groupValue : root.array()) {
            const auto& group = groupValue.array();
            if (group.size() < 2 || !group[0].isNumber() || !group[1].isArray()) {
                std::cerr << "warning: malformed event group skipped\n"; continue;
            }
            const auto time = milliseconds(group[0].number());
            for (const Json& itemValue : group[1].array()) {
                const auto& item = itemValue.array(); if (item.empty()) continue;
                events_.push_back({time, eventSequence_++, scalar(item[0]),
                                   item.size() > 1 ? item[1] : Json(std::string()),
                                   item.size() > 2 ? item[2] : Json(std::string())});
            }
        }
    }

    void emit(std::uint64_t time, std::uint64_t sustain, long long rawLane,
              bool sectionMustHit, std::uint8_t type) {
        const long long keys = options_.lanesPerSide;
        const long long local = rawLane % keys;
        if (rawLane < 0 || local < 0) throw Error("invalid lane");
        const bool otherSide = ((rawLane / keys) & 1LL) != 0;
        const bool mustHit = sectionMustHit != otherSide;
        const std::uint32_t global = mustHit ? static_cast<std::uint32_t>(local)
            : options_.lanesPerSide + static_cast<std::uint32_t>(local);
        if (global > psychgpu::LANE_INDEX_MASK) throw Error("lane exceeds 7-bit global space");
        const std::uint8_t lane = static_cast<std::uint8_t>(global) |
            (mustHit ? psychgpu::LANE_MUST_HIT_FLAG : 0);
        if (sustain <= psychgpu::MAX_SUSTAIN_MS) {
            sorter_.push({{time, static_cast<std::uint16_t>(sustain), lane, type}, sequence_++});
            return;
        }
        ++splitHolds_;
        std::cerr << "warning: hold at " << time << " ms is " << sustain
                  << " ms; splitting into chained segments\n";
        bool first = true;
        while (sustain) {
            const auto length = static_cast<std::uint16_t>(
                std::min<std::uint64_t>(sustain, psychgpu::MAX_SUSTAIN_MS));
            sorter_.push({{time, length, lane,
                           first ? type : psychgpu::NOTE_TYPE_CONTINUATION}, sequence_++});
            first = false; time += length; sustain -= length;
        }
    }

    std::uint8_t noteType(const Json& value) {
        const std::string name = scalar(value);
        if (name.empty() || name == "0" || name == "Default") return psychgpu::NOTE_TYPE_NORMAL;
        const auto found = typeIds_.find(name); if (found != typeIds_.end()) return found->second;
        if (types_.size() >= psychgpu::NOTE_TYPE_CUSTOM_MAX)
            throw Error("more than 254 custom note types; 0xff is reserved");
        const auto id = static_cast<std::uint8_t>(types_.size() + 1);
        types_.push_back(name); typeIds_.emplace(name, id); return id;
    }

    static std::string scalar(const Json& value) {
        if (value.isString()) return value.string();
        if (value.isBool()) return value.boolean() ? "true" : "false";
        if (value.isNumber()) { std::ostringstream s; s << std::setprecision(17) << value.number(); return s.str(); }
        return {};
    }

    static Json exactInteger(std::uint64_t value) {
        if (value <= MAX_SAFE_JSON_INTEGER) return Json(static_cast<double>(value));
        return Json(std::to_string(value));
    }

    void writeInfo(const fs::path& path) {
        Json::Object root;
        root["format"] = Json(std::string("psych-gpu-chart"));
        root["version"] = Json(1.0);
        root["notes_file"] = Json(std::string("notes.bin"));
        root["endianness"] = Json(std::string("little"));
        root["record_size"] = Json(12.0);
        root["note_count"] = exactInteger(noteCount_);
        root["source_note_count"] = exactInteger(sourceNotes_);
        root["timestamp_unit"] = Json(std::string("milliseconds"));
        root["timestamp_rounding"] = Json(std::string("nearest, halfway away from zero"));
        root["lanes_per_side"] = Json(static_cast<double>(options_.lanesPerSide));
        Json::Object lane;
        lane["index_mask"] = Json(127.0); lane["must_hit_flag"] = Json(128.0);
        lane["player_base"] = Json(0.0);
        lane["opponent_base"] = Json(static_cast<double>(options_.lanesPerSide));
        root["lane_encoding"] = Json(std::move(lane));
        Json::Object dictionary; dictionary["Default"] = Json(0.0);
        for (std::size_t i = 0; i < types_.size(); ++i) dictionary[types_[i]] = Json(static_cast<double>(i + 1));
        dictionary["__SustainContinuation"] = Json(255.0);
        root["note_types"] = Json(std::move(dictionary));
        Json::Array eventArray;
        for (const Event& e : events_) {
            Json::Object item; item["timestamp_ms"] = exactInteger(e.timestamp);
            item["name"] = Json(e.name); item["value1"] = e.value1; item["value2"] = e.value2;
            eventArray.emplace_back(std::move(item));
        }
        root["events"] = Json(std::move(eventArray));
        root["metadata"] = Json(metadata_);
        fs::create_directories(path.parent_path());
        const fs::path partial = path.string() + ".partial";
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("cannot create " + partial.string());
        writeJson(out, Json(std::move(root))); out.put('\n'); out.flush();
        if (!out) throw Error("failed writing " + partial.string()); out.close();
        std::error_code ec; fs::remove(path, ec); ec.clear(); fs::rename(partial, path, ec);
        if (ec) throw Error("cannot install " + path.string() + ": " + ec.message());
    }
};

Options arguments(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take = [&](const char* name) -> std::string {
            if (++i >= argc) throw Error(std::string("missing value for ") + name);
            return argv[i];
        };
        if (arg == "-i" || arg == "--input") o.input = take("--input");
        else if (arg == "-o" || arg == "--output") o.output = take("--output");
        else if (arg == "--events") o.events = fs::path(take("--events"));
        else if (arg == "--temp") o.temporary = take("--temp");
        else if (arg == "--chunk-records") o.chunkRecords = std::stoull(take("--chunk-records"));
        else if (arg == "--merge-fan-in") o.mergeFanIn = std::stoull(take("--merge-fan-in"));
        else if (arg == "--lanes-per-side") o.lanesPerSide = std::stoul(take("--lanes-per-side"));
        else if (arg == "--keep-temp") o.keepTemporary = true;
        else if (arg == "-h" || arg == "--help") {
            std::cout <<
                "Usage: chart_compiler -i CHART.json -o DIRECTORY [options]\n"
                "  --events PATH          optional separate events JSON\n"
                "  --temp PATH            temporary directory\n"
                "  --chunk-records N      default 1000000\n"
                "  --merge-fan-in N       default 64\n"
                "  --lanes-per-side N     default 4\n"
                "  --keep-temp            retain merge/spool files\n";
            std::exit(EXIT_SUCCESS);
        } else throw Error("unknown option: " + arg);
    }
    if (o.input.empty()) throw Error("--input is required");
    if (o.output.empty()) throw Error("--output is required");
    if (o.temporary.empty()) o.temporary = o.output / ".chart-compiler-tmp";
    return o;
}

} // namespace

int main(int argc, char** argv) {
    try { Compiler(arguments(argc, argv)).run(); return EXIT_SUCCESS; }
    catch (const std::exception& e) {
        std::cerr << "chart_compiler: error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
