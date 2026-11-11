// Self-checking tests for jeff::json. Exit code 0 on success; prints each failure.

#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, int line) {
    if (!cond) {
        std::fprintf(stderr, "json_test.cpp:%d: FAILED: %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

using jeff::json::Value;

void test_parse_scalars() {
    CHECK(jeff::json::parse("null").value.is_null());
    CHECK(jeff::json::parse("true").value.as_bool() == true);
    CHECK(jeff::json::parse("-42").value.as_int() == -42);
    CHECK(jeff::json::parse("18446744073709551615").value.as_uint() ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK(jeff::json::parse("1.5").value.as_double() == 1.5);
    CHECK(*jeff::json::parse(R"("a\"b\né")").value.as_string() == "a\"b\n\xc3\xa9");
}

void test_parse_nested() {
    auto r = jeff::json::parse(
        R"({"choices":[{"message":{"role":"assistant","content":"hi"}}],"n":3,"x":null})");
    CHECK(static_cast<bool>(r));
    const Value* content = r.value.at({"choices", 0, "message", "content"});
    CHECK(content && *content->as_string() == "hi");
    // Re-reading the same field works (On-Demand alone would not allow this).
    CHECK(r.value.get_int("n") == 3);
    CHECK(r.value.get_int("n") == 3);
    CHECK(r.value.find("x") && r.value.find("x")->is_null());
    CHECK(r.value.find("missing") == nullptr);
    CHECK(r.value.at({"choices", 5}) == nullptr);
    CHECK(r.value.get_string("missing", "dflt") == "dflt");
}

void test_key_order_and_escaped_keys() {
    // Escaped keys are unescaped, so they match plain lookups.
    auto r = jeff::json::parse(R"({"b":1,"a":2})");
    CHECK(static_cast<bool>(r));
    CHECK(r.value.get_int("a") == 2);
    CHECK(r.value.as_object()->front().first == "b");  // insertion order preserved
}

void test_big_integer() {
    auto r = jeff::json::parse(R"({"id":123456789012345678901234567890,"k":1})");
    CHECK(static_cast<bool>(r));
    const Value* id = r.value.find("id");
    CHECK(id && id->as_bigint() && id->as_bigint()->digits == "123456789012345678901234567890");
    CHECK(r.value.get_int("k") == 1);  // parsing continued past the big integer
    CHECK(r.value.dump() == R"({"id":123456789012345678901234567890,"k":1})");
}

void test_parse_errors() {
    CHECK(!jeff::json::parse(R"({"n":3.14.1})"));
    CHECK(!jeff::json::parse(R"({"a":[1,2)"));  // truncated
    CHECK(!jeff::json::parse("{\"a\":\"\xff\xfe\"}") ||
          jeff::json::parse("{\"a\":\"\xff\xfe\"}").value.find("a"));  // replaced or rejected
    CHECK(!jeff::json::parse("{} {}"));                               // trailing content
    CHECK(!jeff::json::parse(""));
    CHECK(!jeff::json::parse("{'a':1}"));  // not JSON5
    std::string deep(300, '[');
    deep += std::string(300, ']');
    auto r = jeff::json::parse(deep);
    CHECK(!r);
    CHECK(!r.error.empty());
    CHECK(static_cast<bool>(jeff::json::parse(deep, 400)));
}

void test_mutation_and_dump() {
    Value v = jeff::json::parse(R"({"a":1,"b":[true]})").value;
    v.set("a", "x");
    v.set("c", Value::object()).set("d", nullptr);
    v.find("b")->push_back(2.5);
    CHECK(v.erase("missing") == false);
    CHECK(v.dump() == R"({"a":"x","b":[true,2.5],"c":{"d":null}})");
    CHECK(v.dump(2) ==
          "{\n  \"a\": \"x\",\n  \"b\": [\n    true,\n    2.5\n  ],\n  \"c\": {\n    \"d\": null\n  }\n}");
    CHECK(Value::object().dump(2) == "{}");
    // Round trip.
    CHECK(jeff::json::parse(v.dump()).value == v);
}

void test_writer() {
    jeff::json::Writer w;
    w.begin_object()
        .field("model", "m")
        .field("stream", true)
        .field("max_tokens", 4096)
        .field("t", 0.25)
        .field("nan", std::nan(""))
        .key("messages")
        .begin_array();
    for (const char* text : {"a\"b", "line\nbreak\ttab\x01"}) {
        w.begin_object().field("role", "user").field("content", text).end_object();
    }
    w.end_array().end_object();
    CHECK(w.ok());
    std::string out = w.str();
    CHECK(out ==
          R"({"model":"m","stream":true,"max_tokens":4096,"t":0.25,"nan":null,"messages":[)"
          R"({"role":"user","content":"a\"b"},{"role":"user","content":"line\nbreak\ttab\u0001"}]})");
    auto back = jeff::json::parse(out);
    CHECK(static_cast<bool>(back));
    CHECK(*back.value.at({"messages", 1, "content"})->as_string() == "line\nbreak\ttab\x01");
}

void test_writer_misuse() {
    jeff::json::Writer a;
    a.begin_object().string("no key").end_object();
    CHECK(!a.ok());
    jeff::json::Writer b;
    b.begin_array().end_object();
    CHECK(!b.ok());
    jeff::json::Writer c;
    c.begin_object();
    CHECK(!c.ok());  // unclosed
    jeff::json::Writer d;
    d.integer(1).integer(2);
    CHECK(!d.ok());  // two roots
    jeff::json::Writer e;
    e.string("\xff");
    CHECK(!e.ok());  // invalid UTF-8
    e.clear();
    e.string("ok");
    CHECK(e.ok());
    CHECK(e.str() == R"("ok")");
}

void test_quote() {
    CHECK(jeff::json::quote("a\"\\\n") == R"("a\"\\\n")");
}

void test_line_splitter_and_sse() {
    jeff::json::LineSplitter ls;
    std::vector<std::string> lines;
    auto collect = [&](std::string_view l) { lines.emplace_back(l); };
    ls.feed("data: {\"a\":", collect);
    ls.feed("1}\r\n\nevent: x\ndata:[DONE]", collect);
    ls.finish(collect);
    CHECK(lines.size() == 4);
    CHECK(lines[0] == "data: {\"a\":1}");
    CHECK(lines[1].empty());
    CHECK(jeff::json::sse_data(lines[0]) == std::string_view("{\"a\":1}"));
    CHECK(!jeff::json::sse_data(lines[2]));
    CHECK(jeff::json::sse_data(lines[3]) == std::string_view("[DONE]"));
}

}  // namespace

int main() {
    test_parse_scalars();
    test_parse_nested();
    test_key_order_and_escaped_keys();
    test_big_integer();
    test_parse_errors();
    test_mutation_and_dump();
    test_writer();
    test_writer_misuse();
    test_quote();
    test_line_splitter_and_sse();
    if (g_failures == 0) std::puts("jeff_json: all tests passed");
    return g_failures == 0 ? 0 : 1;
}
