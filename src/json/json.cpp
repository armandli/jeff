#include "json.h"

#include <cmath>
#include <limits>

#include <simdjson.h>

namespace jeff::json {

namespace od = simdjson::ondemand;

// ---- Value ------------------------------------------------------------------

bool Value::is_number() const noexcept {
    switch (type()) {
        case Type::Int:
        case Type::Uint:
        case Type::Double:
        case Type::BigInt:
            return true;
        default:
            return false;
    }
}

std::optional<bool> Value::as_bool() const noexcept {
    if (auto* b = std::get_if<bool>(&v_)) return *b;
    return std::nullopt;
}

std::optional<std::int64_t> Value::as_int() const noexcept {
    if (auto* i = std::get_if<std::int64_t>(&v_)) return *i;
    if (auto* u = std::get_if<std::uint64_t>(&v_)) {
        if (*u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return static_cast<std::int64_t>(*u);
        return std::nullopt;
    }
    if (auto* d = std::get_if<double>(&v_)) {
        // Exact integral doubles only; 2^63 itself is out of range.
        if (std::trunc(*d) == *d && *d >= -0x1p63 && *d < 0x1p63)
            return static_cast<std::int64_t>(*d);
    }
    return std::nullopt;
}

std::optional<std::uint64_t> Value::as_uint() const noexcept {
    if (auto* u = std::get_if<std::uint64_t>(&v_)) return *u;
    if (auto* i = std::get_if<std::int64_t>(&v_)) {
        if (*i >= 0) return static_cast<std::uint64_t>(*i);
        return std::nullopt;
    }
    if (auto* d = std::get_if<double>(&v_)) {
        if (std::trunc(*d) == *d && *d >= 0 && *d < 0x1p64) return static_cast<std::uint64_t>(*d);
    }
    return std::nullopt;
}

std::optional<double> Value::as_double() const noexcept {
    if (auto* d = std::get_if<double>(&v_)) return *d;
    if (auto* i = std::get_if<std::int64_t>(&v_)) return static_cast<double>(*i);
    if (auto* u = std::get_if<std::uint64_t>(&v_)) return static_cast<double>(*u);
    if (auto* b = std::get_if<BigInt>(&v_)) return std::strtod(b->digits.c_str(), nullptr);
    return std::nullopt;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (auto* o = as_object())
        for (const auto& [k, v] : *o)
            if (k == key) return &v;
    return nullptr;
}

Value* Value::find(std::string_view key) noexcept {
    return const_cast<Value*>(std::as_const(*this).find(key));
}

Value& Value::set(std::string key, Value v) {
    if (is_null()) v_ = Object{};
    auto* o = as_object();
    if (!o) {
        v_ = Object{};
        o = as_object();
    }
    for (auto& [k, existing] : *o)
        if (k == key) return existing = std::move(v);
    return o->emplace_back(std::move(key), std::move(v)).second;
}

bool Value::erase(std::string_view key) {
    auto* o = as_object();
    if (!o) return false;
    for (auto it = o->begin(); it != o->end(); ++it) {
        if (it->first == key) {
            o->erase(it);
            return true;
        }
    }
    return false;
}

const Value* Value::at(std::initializer_list<PathStep> path) const noexcept {
    const Value* cur = this;
    for (const auto& step : path) {
        cur = step.is_index ? cur->at(step.index) : cur->find(step.key);
        if (!cur) return nullptr;
    }
    return cur;
}

Value& Value::push_back(Value v) {
    if (is_null()) v_ = Array{};
    auto* a = as_array();
    if (!a) {
        v_ = Array{};
        a = as_array();
    }
    return a->emplace_back(std::move(v));
}

const Value* Value::at(std::size_t i) const noexcept {
    if (auto* a = as_array(); a && i < a->size()) return &(*a)[i];
    return nullptr;
}

std::size_t Value::size() const noexcept {
    if (auto* a = as_array()) return a->size();
    if (auto* o = as_object()) return o->size();
    return 0;
}

std::string Value::get_string(std::string_view key, std::string_view dflt) const {
    if (auto* v = find(key))
        if (auto* s = v->as_string()) return *s;
    return std::string(dflt);
}

double Value::get_double(std::string_view key, double dflt) const {
    if (auto* v = find(key))
        if (auto d = v->as_double()) return *d;
    return dflt;
}

std::int64_t Value::get_int(std::string_view key, std::int64_t dflt) const {
    if (auto* v = find(key))
        if (auto i = v->as_int()) return *i;
    return dflt;
}

std::optional<bool> Value::get_bool(std::string_view key) const {
    if (auto* v = find(key)) return v->as_bool();
    return std::nullopt;
}

std::string Value::dump(int indent) const {
    Writer w(indent);
    w.value(*this);
    return w.str();
}

// ---- parse ------------------------------------------------------------------

namespace {

// Reads one value from an ondemand::document or ondemand::value (same accessor names)
// into an owning Value. Recursion is bounded by max_depth.
class Converter {
public:
    explicit Converter(std::size_t max_depth) : max_depth_(max_depth) {}

    template <typename Src>
    simdjson::error_code convert(Src& src, Value& out, std::size_t depth) {
        od::json_type t;
        if (auto e = src.type().get(t)) return e;
        switch (t) {
            case od::json_type::object:
                return convert_object(src, out, depth);
            case od::json_type::array:
                return convert_array(src, out, depth);
            case od::json_type::string: {
                std::string_view sv;
                if (auto e = src.get_string(true).get(sv)) return e;
                out = Value(std::string(sv));  // copy: sv dies with the next string read
                return simdjson::SUCCESS;
            }
            case od::json_type::boolean: {
                bool b;
                if (auto e = src.get_bool().get(b)) return e;
                out = Value(b);
                return simdjson::SUCCESS;
            }
            case od::json_type::null: {
                bool is_null;
                if (auto e = src.is_null().get(is_null)) return e;
                if (!is_null) return simdjson::INCORRECT_TYPE;
                out = Value();
                return simdjson::SUCCESS;
            }
            case od::json_type::number:
                return convert_number(src, out);
            default:
                return simdjson::INCORRECT_TYPE;
        }
    }

private:
    template <typename Src>
    simdjson::error_code convert_number(Src& src, Value& out) {
        od::number_type nt;
        if (auto e = src.get_number_type().get(nt)) return e;
        switch (nt) {
            case od::number_type::signed_integer: {
                std::int64_t i;
                if (auto e = src.get_int64().get(i)) return e;
                out = Value(i);
                return simdjson::SUCCESS;
            }
            case od::number_type::unsigned_integer: {
                std::uint64_t u;
                if (auto e = src.get_uint64().get(u)) return e;
                out = Value(u);
                return simdjson::SUCCESS;
            }
            case od::number_type::floating_point_number: {
                double d;
                if (auto e = src.get_double().get(d)) return e;
                out = Value(d);
                return simdjson::SUCCESS;
            }
            case od::number_type::big_integer: {
                // simdjson will not parse > 64-bit integers; keep the literal digits.
                std::string_view tok;
                if constexpr (std::is_same_v<Src, od::document>) {
                    if (auto e = src.raw_json_token().get(tok)) return e;
                } else {
                    tok = src.raw_json_token();
                }
                while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t' ||
                                        tok.back() == '\n' || tok.back() == '\r'))
                    tok.remove_suffix(1);
                out = Value(BigInt{std::string(tok)});
                // raw_json_token() does not advance the iterator. Consume the token as a
                // (lossy) double; if that is refused, On-Demand skips the unread value when
                // the enclosing container moves on, so the error is ignorable.
                double ignored;
                (void)src.get_double().get(ignored);
                return simdjson::SUCCESS;
            }
        }
        return simdjson::INCORRECT_TYPE;
    }

    template <typename Src>
    simdjson::error_code convert_object(Src& src, Value& out, std::size_t depth) {
        if (depth >= max_depth_) return simdjson::DEPTH_ERROR;
        od::object obj;
        if (auto e = src.get_object().get(obj)) return e;
        Value::Object members;
        for (auto field_result : obj) {
            od::field field;
            if (auto e = std::move(field_result).get(field)) return e;
            std::string_view key;
            if (auto e = field.unescaped_key(true).get(key)) return e;
            std::string owned_key(key);  // copy before the value read reuses the buffer
            Value child;
            od::value child_src = field.value();
            if (auto e = convert(child_src, child, depth + 1)) return e;
            members.emplace_back(std::move(owned_key), std::move(child));
        }
        out = Value(std::move(members));
        return simdjson::SUCCESS;
    }

    template <typename Src>
    simdjson::error_code convert_array(Src& src, Value& out, std::size_t depth) {
        if (depth >= max_depth_) return simdjson::DEPTH_ERROR;
        od::array arr;
        if (auto e = src.get_array().get(arr)) return e;
        Value::Array elems;
        for (auto elem_result : arr) {
            od::value elem;
            if (auto e = std::move(elem_result).get(elem)) return e;
            Value child;
            if (auto e = convert(elem, child, depth + 1)) return e;
            elems.push_back(std::move(child));
        }
        out = Value(std::move(elems));
        return simdjson::SUCCESS;
    }

    std::size_t max_depth_;
};

od::parser& thread_parser() {
    // One parser per thread: it is not thread-safe, and reusing it avoids reallocations.
    thread_local od::parser parser;
    return parser;
}

}  // namespace

ParseResult parse(std::string_view text, std::size_t max_depth) {
    ParseResult r;
    // The copy supplies the SIMDJSON_PADDING bytes simdjson reads past the end, and keeps
    // the input alive for the whole conversion regardless of the caller's buffer.
    simdjson::padded_string padded(text.data(), text.size());
    od::document doc;
    if (auto e = thread_parser().iterate(padded).get(doc)) {
        r.error = simdjson::error_message(e);
        return r;
    }
    Converter conv(max_depth);
    if (auto e = conv.convert(doc, r.value, 0)) {
        r.value = Value();
        r.error = simdjson::error_message(e);
        return r;
    }
    if (!doc.at_end()) {
        r.value = Value();
        r.error = "trailing content after JSON document";
    }
    return r;
}

std::string quote(std::string_view s) {
    simdjson::builder::string_builder sb(s.size() + 16);
    sb.escape_and_append_with_quotes(s);
    std::string_view out;
    if (sb.view().get(out)) return {};
    return std::string(out);
}

// ---- Writer -----------------------------------------------------------------

struct Writer::Impl {
    struct Frame {
        bool is_object;
        bool empty = true;
        bool awaiting_value = false;  // object: a key was written, its value is due
    };

    explicit Impl(int indent, std::size_t capacity) : sb(capacity), indent(indent) {}

    simdjson::builder::string_builder sb;
    std::vector<Frame> stack;
    int indent;
    bool root_done = false;
    bool broken = false;

    void newline(std::size_t depth) {
        if (indent < 0) return;
        sb.append_raw("\n");
        for (std::size_t i = 0; i < depth * static_cast<std::size_t>(indent); ++i)
            sb.append_raw(" ");
    }

    // Emits whatever separator must precede a value in the current position.
    void before_value() {
        if (stack.empty()) {
            if (root_done) broken = true;
            return;
        }
        Frame& f = stack.back();
        if (f.is_object) {
            if (!f.awaiting_value) broken = true;  // value without a key
            f.awaiting_value = false;
            return;
        }
        if (!f.empty) sb.append_comma();
        f.empty = false;
        newline(stack.size());
    }

    void after_value() {
        if (stack.empty()) root_done = true;
    }

    void open(bool is_object) {
        before_value();
        if (is_object) sb.start_object();
        else sb.start_array();
        stack.push_back({is_object});
    }

    void close(bool is_object) {
        if (stack.empty() || stack.back().is_object != is_object ||
            stack.back().awaiting_value) {
            broken = true;
            return;
        }
        bool was_empty = stack.back().empty;
        stack.pop_back();
        if (!was_empty) newline(stack.size());
        if (is_object) sb.end_object();
        else sb.end_array();
        after_value();
    }
};

Writer::Writer(int indent, std::size_t capacity)
    : impl_(std::make_unique<Impl>(indent, capacity)) {}
Writer::~Writer() = default;
Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;

Writer& Writer::begin_object() {
    impl_->open(true);
    return *this;
}

Writer& Writer::end_object() {
    impl_->close(true);
    return *this;
}

Writer& Writer::begin_array() {
    impl_->open(false);
    return *this;
}

Writer& Writer::end_array() {
    impl_->close(false);
    return *this;
}

Writer& Writer::key(std::string_view k) {
    auto& im = *impl_;
    if (im.stack.empty() || !im.stack.back().is_object || im.stack.back().awaiting_value) {
        im.broken = true;
        return *this;
    }
    Impl::Frame& f = im.stack.back();
    if (!f.empty) im.sb.append_comma();
    f.empty = false;
    im.newline(im.stack.size());
    im.sb.escape_and_append_with_quotes(k);
    im.sb.append_colon();
    if (im.indent >= 0) im.sb.append_raw(" ");
    f.awaiting_value = true;
    return *this;
}

Writer& Writer::null() {
    impl_->before_value();
    impl_->sb.append_null();
    impl_->after_value();
    return *this;
}

Writer& Writer::boolean(bool b) {
    impl_->before_value();
    impl_->sb.append(b);
    impl_->after_value();
    return *this;
}

Writer& Writer::integer(std::int64_t i) {
    impl_->before_value();
    impl_->sb.append(i);
    impl_->after_value();
    return *this;
}

Writer& Writer::uinteger(std::uint64_t u) {
    impl_->before_value();
    impl_->sb.append(u);
    impl_->after_value();
    return *this;
}

Writer& Writer::number(double d) {
    if (!std::isfinite(d)) return null();
    impl_->before_value();
    impl_->sb.append(d);
    impl_->after_value();
    return *this;
}

Writer& Writer::string(std::string_view s) {
    impl_->before_value();
    impl_->sb.escape_and_append_with_quotes(s);
    impl_->after_value();
    return *this;
}

Writer& Writer::raw(std::string_view json) {
    impl_->before_value();
    impl_->sb.append_raw(json);
    impl_->after_value();
    return *this;
}

Writer& Writer::value(const Value& v) {
    switch (v.type()) {
        case Value::Type::Null:
            return null();
        case Value::Type::Bool:
            return boolean(*v.as_bool());
        case Value::Type::Int:
            return integer(*v.as_int());
        case Value::Type::Uint:
            return uinteger(*v.as_uint());
        case Value::Type::Double:
            return number(*v.as_double());
        case Value::Type::BigInt:
            return raw(v.as_bigint()->digits);
        case Value::Type::String:
            return string(*v.as_string());
        case Value::Type::Array:
            begin_array();
            for (const auto& e : *v.as_array()) value(e);
            return end_array();
        case Value::Type::Object:
            begin_object();
            for (const auto& [k, e] : *v.as_object()) {
                key(k);
                value(e);
            }
            return end_object();
    }
    return *this;
}

bool Writer::ok() const {
    std::string_view out;
    return !impl_->broken && impl_->stack.empty() && impl_->root_done &&
           !impl_->sb.view().get(out) && impl_->sb.validate_unicode();
}

std::string Writer::str() const {
    std::string_view out;
    if (impl_->sb.view().get(out)) return {};
    return std::string(out);
}

void Writer::clear() {
    impl_->sb.clear();
    impl_->stack.clear();
    impl_->root_done = false;
    impl_->broken = false;
}

// ---- SSE --------------------------------------------------------------------

std::optional<std::string_view> sse_data(std::string_view line) noexcept {
    constexpr std::string_view kPrefix = "data:";
    if (line.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    line.remove_prefix(kPrefix.size());
    if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    return line;
}

}  // namespace jeff::json
