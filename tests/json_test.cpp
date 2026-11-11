#include "json.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using jeff::json::Value;
using jeff::json::Writer;

Value parse_ok(std::string_view text) {
    auto r = jeff::json::parse(text);
    EXPECT_TRUE(r) << "parse failed: " << r.error << "\n  input: " << text;
    return std::move(r.value);
}

// ---- parse ------------------------------------------------------------------

TEST(JsonParse, Scalars) {
    EXPECT_TRUE(parse_ok("null").is_null());
    EXPECT_EQ(parse_ok("true").as_bool(), true);
    EXPECT_EQ(parse_ok("-42").as_int(), -42);
    EXPECT_EQ(parse_ok("18446744073709551615").as_uint(),
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(parse_ok("1.5").as_double(), 1.5);
}

TEST(JsonParse, StringEscapesAreDecoded) {
    Value v = parse_ok(R"("a\"b\né")");
    ASSERT_TRUE(v.is_string());
    EXPECT_EQ(*v.as_string(), "a\"b\n\xc3\xa9");
}

TEST(JsonParse, NestedPathLookup) {
    Value v = parse_ok(
        R"({"choices":[{"message":{"role":"assistant","content":"hi"}}],"n":3,"x":null})");
    const Value* content = v.at({"choices", 0, "message", "content"});
    ASSERT_NE(content, nullptr);
    EXPECT_EQ(*content->as_string(), "hi");
    EXPECT_EQ(v.at({"choices", 5}), nullptr);
    EXPECT_EQ(v.at({"choices", 0, "missing"}), nullptr);
}

TEST(JsonParse, FieldsCanBeReadRepeatedly) {
    // simdjson On-Demand consumes each value once; Value does not.
    Value v = parse_ok(R"({"n":3})");
    EXPECT_EQ(v.get_int("n"), 3);
    EXPECT_EQ(v.get_int("n"), 3);
}

TEST(JsonParse, GettersFallBackToDefaults) {
    Value v = parse_ok(R"({"s":"x","n":2,"b":false,"z":null})");
    EXPECT_EQ(v.get_string("s"), "x");
    EXPECT_EQ(v.get_string("missing", "dflt"), "dflt");
    EXPECT_EQ(v.get_string("n", "dflt"), "dflt");  // wrong type
    EXPECT_EQ(v.get_double("n"), 2.0);             // int widens to double
    EXPECT_EQ(v.get_bool("b"), false);
    EXPECT_FALSE(v.get_bool("z").has_value());
    ASSERT_NE(v.find("z"), nullptr);
    EXPECT_TRUE(v.find("z")->is_null());
    EXPECT_EQ(v.find("missing"), nullptr);
}

TEST(JsonParse, EscapedKeysMatchPlainLookups) {
    Value v = parse_ok(R"({"b":1,"a":2})");
    EXPECT_EQ(v.get_int("a"), 2);
}

TEST(JsonParse, ObjectKeyOrderIsPreserved) {
    Value v = parse_ok(R"({"b":1,"a":2})");
    ASSERT_TRUE(v.is_object());
    EXPECT_EQ(v.as_object()->front().first, "b");
}

TEST(JsonParse, BigIntegerKeepsDigitsAndParsingContinues) {
    Value v = parse_ok(R"({"id":123456789012345678901234567890,"k":1})");
    const Value* id = v.find("id");
    ASSERT_NE(id, nullptr);
    ASSERT_NE(id->as_bigint(), nullptr);
    EXPECT_EQ(id->as_bigint()->digits, "123456789012345678901234567890");
    EXPECT_EQ(v.get_int("k"), 1);
    EXPECT_EQ(v.dump(), R"({"id":123456789012345678901234567890,"k":1})");
}

TEST(JsonParse, RejectsMalformedInput) {
    for (std::string_view bad : {R"({"n":3.14.1})", R"({"a":[1,2)", "{} {}", "", "{'a':1}"}) {
        auto r = jeff::json::parse(bad);
        EXPECT_FALSE(r) << "accepted: " << bad;
        EXPECT_FALSE(r.error.empty());
        EXPECT_TRUE(r.value.is_null());
    }
}

TEST(JsonParse, InvalidUtf8IsReplacedOrRejectedWithoutCrashing) {
    auto r = jeff::json::parse("{\"a\":\"\xff\xfe\"}");
    if (r) EXPECT_NE(r.value.find("a"), nullptr);
}

TEST(JsonParse, DepthLimit) {
    std::string deep = std::string(300, '[') + std::string(300, ']');
    auto r = jeff::json::parse(deep);
    EXPECT_FALSE(r);
    EXPECT_FALSE(r.error.empty());
    EXPECT_TRUE(jeff::json::parse(deep, 400));
}

// ---- Value mutation and dump ----------------------------------------------------

TEST(JsonValue, MutationAndCompactDump) {
    Value v = parse_ok(R"({"a":1,"b":[true]})");
    v.set("a", "x");
    v.set("c", Value::object()).set("d", nullptr);
    v.find("b")->push_back(2.5);
    EXPECT_FALSE(v.erase("missing"));
    EXPECT_EQ(v.dump(), R"({"a":"x","b":[true,2.5],"c":{"d":null}})");
    EXPECT_TRUE(v.erase("c"));
    EXPECT_EQ(v.dump(), R"({"a":"x","b":[true,2.5]})");
}

TEST(JsonValue, PrettyDump) {
    Value v = parse_ok(R"({"a":"x","b":[true,2.5],"c":{"d":null}})");
    EXPECT_EQ(v.dump(2),
              "{\n  \"a\": \"x\",\n  \"b\": [\n    true,\n    2.5\n  ],\n"
              "  \"c\": {\n    \"d\": null\n  }\n}");
    EXPECT_EQ(Value::object().dump(2), "{}");
    EXPECT_EQ(Value::array().dump(2), "[]");
}

TEST(JsonValue, RoundTrip) {
    Value v = parse_ok(R"({"s":"line\nbreak","n":-1,"u":18446744073709551615,"d":0.5,)"
                       R"("a":[null,false,{}],"o":{"k":[]}})");
    EXPECT_EQ(parse_ok(v.dump()), v);
    EXPECT_EQ(parse_ok(v.dump(4)), v);
}

TEST(JsonValue, NumericConversions) {
    EXPECT_EQ(Value(3.0).as_int(), 3);
    EXPECT_FALSE(Value(3.5).as_int().has_value());
    EXPECT_FALSE(Value(-1).as_uint().has_value());
    EXPECT_FALSE(Value(std::numeric_limits<std::uint64_t>::max()).as_int().has_value());
    EXPECT_FALSE(Value("3").as_int().has_value());
}

// ---- Writer -------------------------------------------------------------------

TEST(JsonWriter, InsertsSeparatorsAndEscapes) {
    Writer w;
    w.begin_object()
        .field("model", "m")
        .field("stream", true)
        .field("max_tokens", 4096)
        .field("t", 0.25)
        .key("messages")
        .begin_array();
    for (const char* text : {"a\"b", "line\nbreak\ttab\x01"})
        w.begin_object().field("role", "user").field("content", text).end_object();
    w.end_array().end_object();

    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.str(),
              R"({"model":"m","stream":true,"max_tokens":4096,"t":0.25,"messages":[)"
              R"({"role":"user","content":"a\"b"},)"
              R"({"role":"user","content":"line\nbreak\ttab\u0001"}]})");

    Value back = parse_ok(w.str());
    EXPECT_EQ(*back.at({"messages", 1, "content"})->as_string(), "line\nbreak\ttab\x01");
}

TEST(JsonWriter, NonFiniteNumbersBecomeNull) {
    Writer w;
    w.begin_array()
        .value(std::nan(""))
        .value(std::numeric_limits<double>::infinity())
        .end_array();
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.str(), "[null,null]");
}

TEST(JsonWriter, RawInsertsVerbatim) {
    Writer w;
    w.begin_object().key("sub").raw(R"({"x":[1]})").field("y", 2).end_object();
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(w.str(), R"({"sub":{"x":[1]},"y":2})");
}

TEST(JsonWriter, DetectsStructuralMisuse) {
    Writer value_without_key;
    value_without_key.begin_object().string("no key").end_object();
    EXPECT_FALSE(value_without_key.ok());

    Writer mismatched;
    mismatched.begin_array().end_object();
    EXPECT_FALSE(mismatched.ok());

    Writer unclosed;
    unclosed.begin_object();
    EXPECT_FALSE(unclosed.ok());

    Writer two_roots;
    two_roots.integer(1).integer(2);
    EXPECT_FALSE(two_roots.ok());

    Writer dangling_key;
    dangling_key.begin_object().key("k").end_object();
    EXPECT_FALSE(dangling_key.ok());

    Writer empty;
    EXPECT_FALSE(empty.ok());
}

TEST(JsonWriter, RejectsInvalidUtf8AndClearResets) {
    Writer w;
    w.string("\xff");
    EXPECT_FALSE(w.ok());
    w.clear();
    w.string("ok");
    EXPECT_TRUE(w.ok());
    EXPECT_EQ(w.str(), R"("ok")");
}

TEST(JsonQuote, EscapesAndQuotes) {
    EXPECT_EQ(jeff::json::quote("a\"\\\n"), R"("a\"\\\n")");
    EXPECT_EQ(jeff::json::quote(""), R"("")");
}

// ---- LineSplitter / SSE ----------------------------------------------------------

TEST(JsonLineSplitter, ReassemblesLinesAcrossChunks) {
    jeff::json::LineSplitter ls;
    std::vector<std::string> lines;
    auto collect = [&](std::string_view l) { lines.emplace_back(l); };
    ls.feed("data: {\"a\":", collect);
    EXPECT_TRUE(lines.empty());
    ls.feed("1}\r\n\nevent: x\ndata:[DONE]", collect);
    ls.finish(collect);
    EXPECT_EQ(lines, (std::vector<std::string>{"data: {\"a\":1}", "", "event: x", "data:[DONE]"}));
}

TEST(JsonLineSplitter, FinishWithoutPendingEmitsNothing) {
    jeff::json::LineSplitter ls;
    int calls = 0;
    auto count = [&](std::string_view) { ++calls; };
    ls.feed("a\n", count);
    ls.finish(count);
    EXPECT_EQ(calls, 1);
}

TEST(JsonSse, ExtractsDataPayload) {
    EXPECT_EQ(jeff::json::sse_data("data: {\"a\":1}"), std::string_view("{\"a\":1}"));
    EXPECT_EQ(jeff::json::sse_data("data:[DONE]"), std::string_view("[DONE]"));
    EXPECT_FALSE(jeff::json::sse_data("event: message").has_value());
    EXPECT_FALSE(jeff::json::sse_data(": keep-alive").has_value());
    EXPECT_FALSE(jeff::json::sse_data("").has_value());
}

}  // namespace
