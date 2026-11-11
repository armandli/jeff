#include <cstring>

#include <sstream>

#include <curl/curl.h>

#include <json.hpp>

#include <jfsh/agent/openai.hh>

namespace jfsh {

static std::size_t curl_write(char* ptr, std::size_t, std::size_t nmemb,
                               void* userdata) {
  static_cast<std::string*>(userdata)->append(ptr, nmemb);
  return nmemb;
}

// The delta text carried by one SSE line of a chat completion stream:
//   data: {"choices":[{"delta":{"content":"..."}}]}
static std::optional<std::string> sse_delta(std::string_view line) {
  auto data = jeff::json::sse_data(line);
  if (not data or *data == "[DONE]") return std::nullopt;
  auto parsed = jeff::json::parse(*data);
  if (not parsed) return std::nullopt;
  const jeff::json::Value* text =
      parsed.value.at({"choices", 0, "delta", "content"});
  if (not text or not text->is_string() or text->as_string()->empty())
    return std::nullopt;
  return *text->as_string();
}

struct StreamState {
  StreamCallback           cb;
  std::string              content;
  std::string              non_sse;  // body of a non-streamed (error) response
  jeff::json::LineSplitter lines;

  void on_line(std::string_view line) {
    if (auto text = sse_delta(line)) {
      content += *text;
      if (cb) cb(*text);
    } else if (not jeff::json::sse_data(line) and not line.empty()
               and line[0] != ':' and line.substr(0, 6) != "event:") {
      non_sse.append(line);
    }
  }
};

static std::size_t curl_stream_write(char* ptr, std::size_t, std::size_t nmemb,
                                     void* userdata) {
  auto* state = static_cast<StreamState*>(userdata);
  state->lines.feed({ptr, nmemb},
                    [state](std::string_view line) { state->on_line(line); });
  return nmemb;
}

OpenAIBackend::OpenAIBackend(ProviderConfig cfg) : mCfg(std::move(cfg)) {}

std::string OpenAIBackend::build_body(const ChatRequest& req) const {
  jeff::json::Writer w;
  w.begin_object()
      .field("model",      req.config.model)
      .field("stream",     req.stream)
      .field("max_tokens", req.config.params.max_tokens)
      .key("messages").begin_array();
  for (const auto& m : req.messages) {
    const char* role = "user";
    if (m.role == Role::System)    role = "system";
    if (m.role == Role::Assistant) role = "assistant";
    w.begin_object().field("role", role).field("content", m.content).end_object();
  }
  w.end_array().end_object();
  return w.str();
}

Result<std::string> OpenAIBackend::http_post(std::string_view url,
                                              std::string_view body,
                                              std::string_view extra_headers) const {
  CURL* curl = curl_easy_init();
  if (not curl)
    return std::unexpected(ShellError{
        ErrorKind::AgentError, "curl_easy_init failed"});

  std::string response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (not mCfg.api_key.empty()) {
    std::string auth = "Authorization: Bearer " + mCfg.api_key;
    headers = curl_slist_append(headers, auth.c_str());
  }
  if (not extra_headers.empty()) {
    // extra_headers is newline-separated
    std::string eh_str{extra_headers};
    std::istringstream ss{eh_str};
    std::string h;
    while (std::getline(ss, h))
      if (not h.empty()) headers = curl_slist_append(headers, h.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL,           std::string(url).c_str());
  // POSTFIELDS is not copied by curl; body outlives curl_easy_perform below.
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.data());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    static_cast<long>(mCfg.timeout_ms));

  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    return std::unexpected(ShellError{ErrorKind::AgentError,
        std::string("curl: ") + curl_easy_strerror(rc)});
  return response;
}

Result<ChatResponse> OpenAIBackend::parse_response(std::string_view json) const {
  // {"choices":[{"message":{"content":"..."},"finish_reason":"stop"}],
  //  "usage":{"prompt_tokens":N,"completion_tokens":M}}
  auto parsed = jeff::json::parse(json);
  if (not parsed)
    return std::unexpected(ShellError{
        ErrorKind::AgentError, "invalid response: " + parsed.error});
  if (auto err = api_error_message(parsed.value))
    return std::unexpected(ShellError{ErrorKind::AgentError, *err});

  const jeff::json::Value* choice = parsed.value.at({"choices", 0});
  const jeff::json::Value* content =
      choice ? choice->at({"message", "content"}) : nullptr;
  if (not content)
    return std::unexpected(ShellError{
        ErrorKind::AgentError, "no content in response"});

  ChatResponse r;
  if (const std::string* text = content->as_string()) r.content = *text;
  r.finish_reason = choice->get_string("finish_reason");
  if (const jeff::json::Value* usage = parsed.value.find("usage")) {
    r.prompt_tokens   = static_cast<int>(usage->get_int("prompt_tokens"));
    r.response_tokens = static_cast<int>(usage->get_int("completion_tokens"));
  }
  return r;
}

Result<ChatResponse> OpenAIBackend::chat(const ChatRequest& req) {
  std::string url = (mCfg.base_url.empty()
      ? "https://api.openai.com/v1"
      : mCfg.base_url)
      + "/chat/completions";
  auto body = build_body(req);
  auto resp = http_post(url, body);
  if (not resp) return std::unexpected(resp.error());
  return parse_response(*resp);
}

Result<ChatResponse> OpenAIBackend::chat_stream(const ChatRequest& req,
                                                 StreamCallback cb) {
  CURL* curl = curl_easy_init();
  if (not curl)
    return std::unexpected(ShellError{
        ErrorKind::AgentError, "curl_easy_init failed"});

  std::string url = (mCfg.base_url.empty()
      ? "https://api.openai.com/v1"
      : mCfg.base_url)
      + "/chat/completions";
  ChatRequest sreq = req;
  sreq.stream = true;
  std::string body = build_body(sreq);

  StreamState state{cb, {}, {}, {}};
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: text/event-stream");
  if (not mCfg.api_key.empty()) {
    std::string auth = "Authorization: Bearer " + mCfg.api_key;
    headers = curl_slist_append(headers, auth.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_stream_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &state);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    static_cast<long>(mCfg.timeout_ms));

  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    return std::unexpected(ShellError{ErrorKind::AgentError,
        std::string("curl: ") + curl_easy_strerror(rc)});

  state.lines.finish([&state](std::string_view line) { state.on_line(line); });
  if (state.content.empty() and not state.non_sse.empty()) {
    if (auto parsed = jeff::json::parse(state.non_sse))
      if (auto err = api_error_message(parsed.value))
        return std::unexpected(ShellError{ErrorKind::AgentError, *err});
  }

  ChatResponse r;
  r.content = std::move(state.content);
  return r;
}

std::optional<std::string>
OpenAIBackend::parse_sse_chunk(std::string_view line) const {
  return sse_delta(line);
}

}  // namespace jfsh
