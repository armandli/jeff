// jeff::json — the repo's JSON layer, built on simdjson.
//
// simdjson is the parser/serializer everywhere; this header fills the gaps it leaves:
//
//   Value        owning, mutable, random-access tree. simdjson On-Demand is forward-only,
//                single-consumption and borrows the parser's buffers; Value copies out so
//                callers can re-read fields, keep them past the parse, edit, and re-emit.
//   parse()      padded copy + per-thread parser + depth cap + trailing-content check,
//                with big integers (> 64 bit) kept as their literal digits.
//   Writer       string_builder wrapper that inserts commas/colons itself, tracks
//                structure, maps NaN/Inf to null, and can pretty-print.
//   LineSplitter reassembles '\n'-delimited records (NDJSON, SSE) from arbitrary network
//                chunks, since simdjson needs each document whole.
//
// simdjson.h is deliberately not included here: it is large and noisy under strict warning
// sets, so it stays inside json.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace jeff::json {

// Integer literal that does not fit in 64 bits; kept verbatim so it round-trips exactly.
struct BigInt {
    std::string digits;
    bool operator==(const BigInt&) const = default;
};

class Value {
public:
    enum class Type { Null, Bool, Int, Uint, Double, BigInt, String, Array, Object };

    using Array  = std::vector<Value>;
    using Member = std::pair<std::string, Value>;
    using Object = std::vector<Member>;  // insertion order preserved

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v_(b) {}
    template <std::integral I>
        requires(!std::same_as<I, bool> && !std::same_as<I, char>)
    Value(I i) {
        if constexpr (std::is_signed_v<I>) v_ = static_cast<std::int64_t>(i);
        else v_ = static_cast<std::uint64_t>(i);
    }
    Value(double d) : v_(d) {}
    Value(float f) : v_(static_cast<double>(f)) {}
    Value(BigInt b) : v_(std::move(b)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(Array a) : v_(std::move(a)) {}
    Value(Object o) : v_(std::move(o)) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Type type() const noexcept { return static_cast<Type>(v_.index()); }
    bool is_null() const noexcept { return type() == Type::Null; }
    bool is_bool() const noexcept { return type() == Type::Bool; }
    bool is_number() const noexcept;
    bool is_string() const noexcept { return type() == Type::String; }
    bool is_array() const noexcept { return type() == Type::Array; }
    bool is_object() const noexcept { return type() == Type::Object; }

    // Typed access: nullopt / nullptr when the value has another type. Numbers convert
    // between int/uint/double when the target can hold the value exactly (double: always).
    std::optional<bool> as_bool() const noexcept;
    std::optional<std::int64_t> as_int() const noexcept;
    std::optional<std::uint64_t> as_uint() const noexcept;
    std::optional<double> as_double() const noexcept;
    const std::string* as_string() const noexcept { return std::get_if<std::string>(&v_); }
    const BigInt* as_bigint() const noexcept { return std::get_if<BigInt>(&v_); }
    const Array* as_array() const noexcept { return std::get_if<Array>(&v_); }
    Array* as_array() noexcept { return std::get_if<Array>(&v_); }
    const Object* as_object() const noexcept { return std::get_if<Object>(&v_); }
    Object* as_object() noexcept { return std::get_if<Object>(&v_); }

    // Object access. find() returns nullptr when absent or not an object; with duplicate
    // keys it returns the first. set() replaces an existing key or appends; on a null value
    // it first turns it into an object.
    const Value* find(std::string_view key) const noexcept;
    Value* find(std::string_view key) noexcept;
    Value& set(std::string key, Value v);
    bool erase(std::string_view key);

    // Path lookup through nested objects/arrays: at({"choices", 0, "message"}).
    struct PathStep {
        PathStep(std::string_view k) : key(k) {}
        PathStep(const char* k) : key(k) {}
        PathStep(std::size_t i) : index(i), is_index(true) {}
        PathStep(int i) : index(static_cast<std::size_t>(i)), is_index(true) {}
        std::string_view key;
        std::size_t index = 0;
        bool is_index = false;
    };
    const Value* at(std::initializer_list<PathStep> path) const noexcept;

    // Array access.
    Value& push_back(Value v);
    const Value* at(std::size_t i) const noexcept;
    std::size_t size() const noexcept;  // elements / members; 0 for scalars

    // Field getters with defaults, for the common "read a flat object" case.
    std::string get_string(std::string_view key, std::string_view dflt = {}) const;
    double get_double(std::string_view key, double dflt = 0.0) const;
    std::int64_t get_int(std::string_view key, std::int64_t dflt = 0) const;
    std::optional<bool> get_bool(std::string_view key) const;

    // Serialization. indent < 0 → compact; otherwise pretty-printed with that many spaces.
    std::string dump(int indent = -1) const;

    bool operator==(const Value&) const = default;

private:
    std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, BigInt,
                 std::string, Array, Object>
        v_;
};

struct ParseResult {
    Value value;
    std::string error;  // empty on success
    explicit operator bool() const noexcept { return error.empty(); }
};

// Parses one complete RFC 8259 document. Invalid UTF-8 in strings and lone surrogates are
// replaced with U+FFFD rather than rejected (LLM output occasionally contains them).
// Nesting beyond max_depth is rejected so hostile input cannot exhaust the stack.
inline constexpr std::size_t kDefaultMaxDepth = 256;
ParseResult parse(std::string_view text, std::size_t max_depth = kDefaultMaxDepth);

// Escapes and quotes s as a JSON string literal.
std::string quote(std::string_view s);

class Writer {
public:
    // indent < 0 → compact output; otherwise pretty-printed.
    explicit Writer(int indent = -1, std::size_t capacity = 1024);
    ~Writer();
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;

    Writer& begin_object();
    Writer& end_object();
    Writer& begin_array();
    Writer& end_array();
    Writer& key(std::string_view k);

    Writer& null();
    Writer& boolean(bool b);
    Writer& integer(std::int64_t i);
    Writer& uinteger(std::uint64_t u);
    Writer& number(double d);  // NaN/Inf have no JSON form and are written as null
    Writer& string(std::string_view s);
    Writer& value(const Value& v);
    Writer& raw(std::string_view json);  // already-serialized JSON, inserted verbatim

    // Generic value(): dispatches on the C++ type.
    template <typename T>
    Writer& value(const T& v) {
        if constexpr (std::is_same_v<T, std::nullptr_t>) return null();
        else if constexpr (std::is_same_v<T, bool>) return boolean(v);
        else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) return integer(v);
        else if constexpr (std::is_integral_v<T>) return uinteger(v);
        else if constexpr (std::is_floating_point_v<T>) return number(static_cast<double>(v));
        else if constexpr (std::is_convertible_v<const T&, std::string_view>)
            return string(std::string_view(v));
        else static_assert(sizeof(T) == 0, "jeff::json::Writer::value: unsupported type");
    }

    template <typename T>
    Writer& field(std::string_view k, const T& v) {
        key(k);
        return value(v);
    }

    // True once exactly one complete top-level value was written with correctly
    // nested calls, and the output is valid UTF-8.
    bool ok() const;
    // The output so far. Valid JSON only when ok().
    std::string str() const;
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Splits a byte stream into '\n'-terminated lines (a trailing '\r' is dropped), carrying
// partial lines across feed() calls. finish() flushes a final unterminated line.
class LineSplitter {
public:
    template <typename F>
    void feed(std::string_view chunk, F&& on_line) {
        std::size_t start = 0;
        for (std::size_t nl; (nl = chunk.find('\n', start)) != std::string_view::npos;
             start = nl + 1) {
            std::string_view piece = chunk.substr(start, nl - start);
            if (pending_.empty()) {
                emit(piece, on_line);
            } else {
                pending_.append(piece);
                emit(pending_, on_line);
                pending_.clear();
            }
        }
        pending_.append(chunk.substr(start));
    }

    template <typename F>
    void finish(F&& on_line) {
        if (!pending_.empty()) emit(pending_, on_line);
        pending_.clear();
    }

private:
    template <typename F>
    static void emit(std::string_view line, F& on_line) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        on_line(line);
    }

    std::string pending_;
};

// Server-Sent Events: the payload of a "data:" line (one optional leading space removed),
// or nullopt for any other line (event:, id:, comments, blanks).
std::optional<std::string_view> sse_data(std::string_view line) noexcept;

}  // namespace jeff::json
