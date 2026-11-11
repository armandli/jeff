#include <curl/curl.h>

#include <json.hpp>

#include <jfsh/agent/anthropic.hh>

namespace jfsh {

static std::size_t curl_write_anthropic(char* ptr, std::size_t, std::size_t nmemb,
                                         void* ud) {
  static_cast<std::string*>(ud)->append(ptr, nmemb);
  return nmemb;
}

AnthropicBackend::AnthropicBackend(ProviderConfig cfg) : mCfg(std::move(cfg)) {}

std::string AnthropicBackend::build_body(const ChatRequest& req) const {
  std::string_view system_text;
  for (const auto& m : req.messages)
    if (m.role == Role::System) system_text = m.content;

  jeff::json::Writer w;
  w.begin_object()
      .field("model",      req.config.model)
      .field("max_tokens", req.config.params.max_tokens);
  if (not system_text.empty()) w.field("system", system_text);
  if (req.stream)              w.field("stream", true);
  w.key("messages").begin_array();
  for (const auto& m : req.messages) {
    if (m.role == Role::System) continue;
    const char* role = m.role == Role::Assistant ? "assistant" : "user";
    w.begin_object().field("role", role).field("content", m.content).end_object();
  }
  w.end_array().end_object();
  return w.str();
}

Result<std::string> AnthropicBackend::http_post(std::string_view url,
                                                 std::string_view body) const {
  CURL* curl = curl_easy_init();
  if (not curl)
    return std::unexpected(ShellError{ErrorKind::AgentError, "curl init failed"});

  std::string resp;
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  std::string ak = "x-api-key: " + mCfg.api_key;
  hdrs = curl_slist_append(hdrs, ak.c_str());
  std::string av = std::string("anthropic-version: ") + std::string(kApiVersion);
  hdrs = curl_slist_append(hdrs, av.c_str());

  curl_easy_setopt(curl, CURLOPT_URL,           std::string(url).c_str());
  // POSTFIELDS is not copied by curl; body outlives curl_easy_perform below.
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.data());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_anthropic);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    static_cast<long>(mCfg.timeout_ms));

  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    return std::unexpected(ShellError{ErrorKind::AgentError,
        std::string("curl: ") + curl_easy_strerror(rc)});
  return resp;
}

Result<ChatResponse> AnthropicBackend::parse_response(std::string_view json) const {
  // {"content":[{"type":"text","text":"..."},...],"stop_reason":"end_turn",
  //  "usage":{"input_tokens":N,"output_tokens":M}}
  auto parsed = jeff::json::parse(json);
  if (not parsed)
    return std::unexpected(ShellError{
        ErrorKind::AgentError, "invalid response: " + parsed.error});
  if (auto err = api_error_message(parsed.value))
    return std::unexpected(ShellError{ErrorKind::AgentError, *err});

  const jeff::json::Value* blocks = parsed.value.find("content");
  if (not blocks or not blocks->is_array())
    return std::unexpected(ShellError{ErrorKind::AgentError, "no content in response"});

  ChatResponse r;
  for (const auto& block : *blocks->as_array())
    if (block.get_string("type") == "text") r.content += block.get_string("text");
  r.finish_reason = parsed.value.get_string("stop_reason");
  if (const jeff::json::Value* usage = parsed.value.find("usage")) {
    r.prompt_tokens   = static_cast<int>(usage->get_int("input_tokens"));
    r.response_tokens = static_cast<int>(usage->get_int("output_tokens"));
  }
  return r;
}

std::optional<std::string>
AnthropicBackend::parse_sse_chunk(std::string_view line) const {
  // data: {"type":"content_block_delta","delta":{"type":"text_delta","text":"..."}}
  auto data = jeff::json::sse_data(line);
  if (not data) return std::nullopt;
  auto parsed = jeff::json::parse(*data);
  if (not parsed or parsed.value.get_string("type") != "content_block_delta")
    return std::nullopt;
  const jeff::json::Value* delta = parsed.value.find("delta");
  if (not delta or delta->get_string("type") != "text_delta") return std::nullopt;
  return delta->get_string("text");
}

Result<ChatResponse> AnthropicBackend::chat(const ChatRequest& req) {
  std::string url = (mCfg.base_url.empty()
      ? "https://api.anthropic.com"
      : mCfg.base_url)
      + "/v1/messages";
  auto body = build_body(req);
  auto resp = http_post(url, body);
  if (not resp) return std::unexpected(resp.error());
  return parse_response(*resp);
}

Result<ChatResponse> AnthropicBackend::chat_stream(const ChatRequest& req,
                                                    StreamCallback cb) {
  // TODO(anthropic/stream): Implement SSE streaming for the Anthropic API.
  //   Anthropic SSE events:
  //     event: content_block_delta  → data: {"type":"content_block_delta","delta":{"text":"..."}}
  //     event: message_stop         → streaming complete
  //   Use a CURLOPT_WRITEFUNCTION callback (like curl_stream_write in openai.cpp)
  //   that feeds a jeff::json::LineSplitter and passes each line to
  //   parse_sse_chunk(), which already decodes content_block_delta events.
  //   Wire CURLOPT_HTTPHEADER with Accept: text/event-stream.
  ChatRequest sreq = req; sreq.stream = true;
  auto resp = chat(sreq);  // fallback to non-streaming until TODO above is done
  if (resp and cb) cb(resp->content);
  return resp;
}

}  // namespace jfsh
