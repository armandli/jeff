#include <string>

#include <gtest/gtest.h>

#include <json.hpp>

#include <jfsh/agent/anthropic.hh>
#include <jfsh/agent/ollama.hh>
#include <jfsh/agent/openai.hh>
#include <jfsh/agent/provider.hh>

namespace jfsh {
namespace {

// The request/response codecs are protected; these probes expose them so they can be
// tested without a network.
struct OpenAIProbe : OpenAIBackend {
  using OpenAIBackend::OpenAIBackend;
  using OpenAIBackend::build_body;
  using OpenAIBackend::parse_response;
  using OpenAIBackend::parse_sse_chunk;
};

struct AnthropicProbe : AnthropicBackend {
  using AnthropicBackend::AnthropicBackend;
  using AnthropicBackend::build_body;
  using AnthropicBackend::parse_response;
  using AnthropicBackend::parse_sse_chunk;
};

struct OllamaProbe : OllamaBackend {
  using OllamaBackend::OllamaBackend;
  using OllamaBackend::build_body;
  using OllamaBackend::parse_response;
  using OllamaBackend::parse_ndjson_chunk;
};

ChatRequest make_request(bool stream = false) {
  ChatRequest r;
  r.config.model             = "m\"odel";
  r.config.params.max_tokens = 256;
  r.stream                   = stream;
  r.messages = {{Role::System,    "be \"brief\"\n\ttab", ""},
                {Role::User,      "ctrl\x01 quote\" back\\slash caf\xc3\xa9", ""},
                {Role::Assistant, "prev", ""},
                {Role::User,      "again", ""}};
  return r;
}

jeff::json::Value parse_body(const std::string& body) {
  auto r = jeff::json::parse(body);
  EXPECT_TRUE(r) << r.error << "\n  body: " << body;
  return std::move(r.value);
}

// ---- OpenAI -----------------------------------------------------------------

TEST(OpenAIBackend, BuildBodyIsValidJsonWithAllMessages) {
  OpenAIProbe b{ProviderConfig{}};
  auto body = parse_body(b.build_body(make_request(/*stream=*/true)));
  EXPECT_EQ(body.get_string("model"), "m\"odel");
  EXPECT_EQ(body.get_bool("stream"), true);
  EXPECT_EQ(body.get_int("max_tokens"), 256);
  const auto* msgs = body.find("messages");
  ASSERT_NE(msgs, nullptr);
  ASSERT_EQ(msgs->size(), 4u);
  EXPECT_EQ(*msgs->at({0, "role"})->as_string(), "system");
  EXPECT_EQ(*msgs->at({0, "content"})->as_string(), "be \"brief\"\n\ttab");
  EXPECT_EQ(*msgs->at({1, "content"})->as_string(),
            "ctrl\x01 quote\" back\\slash caf\xc3\xa9");
  EXPECT_EQ(*msgs->at({2, "role"})->as_string(), "assistant");
}

TEST(OpenAIBackend, ParseResponseReadsContentAndUsage) {
  OpenAIProbe b{ProviderConfig{}};
  auto r = b.parse_response(
      R"({"choices":[{"message":{"role":"assistant","content":"café \"q\"\nx"},)"
      R"("finish_reason":"stop"}],"usage":{"prompt_tokens":11,"completion_tokens":7}})");
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->content, "caf\xc3\xa9 \"q\"\nx");
  EXPECT_EQ(r->finish_reason, "stop");
  EXPECT_EQ(r->prompt_tokens, 11);
  EXPECT_EQ(r->response_tokens, 7);
}

TEST(OpenAIBackend, ParseResponseSurfacesApiError) {
  OpenAIProbe b{ProviderConfig{}};
  auto r = b.parse_response(R"({"error":{"message":"Incorrect API key","type":"auth"}})");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::AgentError);
  EXPECT_EQ(r.error().message, "Incorrect API key");
}

TEST(OpenAIBackend, ParseResponseRejectsGarbageAndMissingContent) {
  OpenAIProbe b{ProviderConfig{}};
  EXPECT_FALSE(b.parse_response("not json").has_value());
  EXPECT_FALSE(b.parse_response(R"({"choices":[]})").has_value());
}

TEST(OpenAIBackend, ParseSseChunk) {
  OpenAIProbe b{ProviderConfig{}};
  EXPECT_EQ(b.parse_sse_chunk(R"(data: {"choices":[{"delta":{"content":"Hel\nlo"}}]})"),
            "Hel\nlo");
  EXPECT_FALSE(b.parse_sse_chunk(R"(data: {"choices":[{"delta":{}}]})").has_value());
  EXPECT_FALSE(b.parse_sse_chunk("data: [DONE]").has_value());
  EXPECT_FALSE(b.parse_sse_chunk("event: ping").has_value());
  EXPECT_FALSE(b.parse_sse_chunk("data: {truncated").has_value());
}

// ---- Anthropic ----------------------------------------------------------------

TEST(AnthropicBackend, BuildBodyLiftsSystemPromptOutOfMessages) {
  AnthropicProbe b{ProviderConfig{}};
  auto body = parse_body(b.build_body(make_request()));
  EXPECT_EQ(body.get_string("system"), "be \"brief\"\n\ttab");
  EXPECT_EQ(body.find("stream"), nullptr);  // only sent when streaming
  const auto* msgs = body.find("messages");
  ASSERT_NE(msgs, nullptr);
  ASSERT_EQ(msgs->size(), 3u);
  EXPECT_EQ(*msgs->at({0, "role"})->as_string(), "user");
  EXPECT_EQ(*msgs->at({1, "role"})->as_string(), "assistant");
}

TEST(AnthropicBackend, ParseResponseConcatenatesTextBlocks) {
  AnthropicProbe b{ProviderConfig{}};
  auto r = b.parse_response(
      R"({"content":[{"type":"text","text":"one, "},{"type":"tool_use","id":"t"},)"
      R"({"type":"text","text":"two"}],"stop_reason":"end_turn",)"
      R"("usage":{"input_tokens":5,"output_tokens":9}})");
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->content, "one, two");
  EXPECT_EQ(r->finish_reason, "end_turn");
  EXPECT_EQ(r->prompt_tokens, 5);
  EXPECT_EQ(r->response_tokens, 9);
}

TEST(AnthropicBackend, ParseResponseSurfacesApiError) {
  AnthropicProbe b{ProviderConfig{}};
  auto r = b.parse_response(
      R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().message, "Overloaded");
}

TEST(AnthropicBackend, ParseSseChunkOnlyReturnsTextDeltas) {
  AnthropicProbe b{ProviderConfig{}};
  EXPECT_EQ(b.parse_sse_chunk(R"(data: {"type":"content_block_delta","index":0,)"
                              R"("delta":{"type":"text_delta","text":"Hi"}})"),
            "Hi");
  EXPECT_FALSE(b.parse_sse_chunk(R"(data: {"type":"content_block_delta",)"
                                 R"("delta":{"type":"input_json_delta","partial_json":"{"}})")
                   .has_value());
  EXPECT_FALSE(b.parse_sse_chunk(R"(data: {"type":"message_stop"})").has_value());
  EXPECT_FALSE(b.parse_sse_chunk("event: content_block_delta").has_value());
}

// ---- Ollama -------------------------------------------------------------------

TEST(OllamaBackend, BuildBodySetsStreamFlag) {
  OllamaProbe b{ProviderConfig{}};
  auto body = parse_body(b.build_body(make_request(), /*stream=*/false));
  EXPECT_EQ(body.get_bool("stream"), false);
  ASSERT_NE(body.find("messages"), nullptr);
  EXPECT_EQ(body.find("messages")->size(), 4u);
  EXPECT_EQ(parse_body(b.build_body(make_request(), true)).get_bool("stream"), true);
}

TEST(OllamaBackend, ParseResponse) {
  OllamaProbe b{ProviderConfig{}};
  auto r = b.parse_response(
      R"({"message":{"role":"assistant","content":"hi 😀"},"done":true,)"
      R"("done_reason":"stop","prompt_eval_count":3,"eval_count":4})");
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->content, "hi \xf0\x9f\x98\x80");
  EXPECT_EQ(r->finish_reason, "stop");
  EXPECT_EQ(r->prompt_tokens, 3);
  EXPECT_EQ(r->response_tokens, 4);
}

TEST(OllamaBackend, ParseResponseSurfacesStringError) {
  OllamaProbe b{ProviderConfig{}};
  auto r = b.parse_response(R"({"error":"model \"llama9\" not found"})");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().message, "model \"llama9\" not found");
}

TEST(OllamaBackend, ParseNdjsonChunk) {
  OllamaProbe b{ProviderConfig{}};
  EXPECT_EQ(b.parse_ndjson_chunk(R"({"message":{"content":"tok"},"done":false})"), "tok");
  EXPECT_FALSE(b.parse_ndjson_chunk(R"({"message":{"content":""},"done":true})").has_value());
  EXPECT_FALSE(b.parse_ndjson_chunk("").has_value());
}

// ---- api_error_message ----------------------------------------------------------

TEST(ApiErrorMessage, Shapes) {
  auto v = [](std::string_view s) { return jeff::json::parse(s).value; };
  EXPECT_EQ(api_error_message(v(R"({"error":{"message":"m"}})")), "m");
  EXPECT_EQ(api_error_message(v(R"({"error":"plain"})")), "plain");
  EXPECT_EQ(api_error_message(v(R"({"error":{"code":42}})")), R"({"code":42})");
  EXPECT_FALSE(api_error_message(v(R"({"error":null})")).has_value());
  EXPECT_FALSE(api_error_message(v(R"({"choices":[]})")).has_value());
}

}  // namespace
}  // namespace jfsh
