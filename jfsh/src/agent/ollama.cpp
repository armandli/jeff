#include <curl/curl.h>

#include <json.hpp>

#include <jfsh/agent/ollama.hh>

namespace jfsh {

static std::size_t curl_write_ollama(char* ptr, std::size_t, std::size_t nmemb,
                                     void* ud) {
  static_cast<std::string*>(ud)->append(ptr, nmemb);
  return nmemb;
}

OllamaBackend::OllamaBackend(ProviderConfig cfg) : mCfg(std::move(cfg)) {
  if (mCfg.base_url.empty()) mCfg.base_url = "http://localhost:11434";
}

bool OllamaBackend::is_server_available() const noexcept {
  CURL* curl = curl_easy_init();
  if (not curl) return false;
  std::string url = mCfg.base_url + "/api/tags";
  curl_easy_setopt(curl, CURLOPT_URL,       url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOBODY,    1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
  CURLcode rc = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK;
}

std::string OllamaBackend::build_body(const ChatRequest& req, bool stream) const {
  jeff::json::Writer w;
  w.begin_object()
      .field("model",  req.config.model)
      .field("stream", stream)
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

Result<std::string> OllamaBackend::http_post(std::string_view url,
                                              std::string_view body) const {
  CURL* curl = curl_easy_init();
  if (not curl)
    return std::unexpected(ShellError{ErrorKind::AgentError, "curl init"});
  std::string resp;
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_URL,           std::string(url).c_str());
  // POSTFIELDS is not copied by curl; body outlives curl_easy_perform below.
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.data());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_ollama);
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

Result<ChatResponse> OllamaBackend::parse_response(std::string_view json) const {
  // {"message":{"role":"assistant","content":"..."},"done_reason":"stop",
  //  "prompt_eval_count":N,"eval_count":M}
  auto parsed = jeff::json::parse(json);
  if (not parsed)
    return std::unexpected(ShellError{
        ErrorKind::AgentError, "invalid response: " + parsed.error});
  if (auto err = api_error_message(parsed.value))
    return std::unexpected(ShellError{ErrorKind::AgentError, *err});

  const jeff::json::Value* content = parsed.value.at({"message", "content"});
  if (not content or not content->is_string())
    return std::unexpected(ShellError{ErrorKind::AgentError, "no content"});

  ChatResponse r;
  r.content         = *content->as_string();
  r.finish_reason   = parsed.value.get_string("done_reason");
  r.prompt_tokens   = static_cast<int>(parsed.value.get_int("prompt_eval_count"));
  r.response_tokens = static_cast<int>(parsed.value.get_int("eval_count"));
  return r;
}

std::optional<std::string>
OllamaBackend::parse_ndjson_chunk(std::string_view line) const {
  // {"message":{"role":"assistant","content":"chunk"},"done":false}
  auto parsed = jeff::json::parse(line);
  if (not parsed) return std::nullopt;
  const jeff::json::Value* content = parsed.value.at({"message", "content"});
  if (not content or not content->is_string() or content->as_string()->empty())
    return std::nullopt;
  return *content->as_string();
}

Result<ChatResponse> OllamaBackend::chat(const ChatRequest& req) {
  std::string url = mCfg.base_url + "/api/chat";
  auto body = build_body(req, false);
  auto resp = http_post(url, body);
  if (not resp) return std::unexpected(resp.error());
  return parse_response(*resp);
}

Result<ChatResponse> OllamaBackend::chat_stream(const ChatRequest& req,
                                                 StreamCallback cb) {
  // TODO(ollama/stream): Implement NDJSON streaming for Ollama.
  //   Ollama streams one JSON object per line, each with:
  //     {"message":{"role":"assistant","content":"chunk"},"done":false}
  //   The final line has "done":true and may include timing metadata.
  //   Use a CURLOPT_WRITEFUNCTION callback that feeds a jeff::json::LineSplitter
  //   and passes each complete line to parse_ndjson_chunk(), which already
  //   extracts .message.content; invoke the StreamCallback with the result.
  //   Wire build_body(req, /*stream=*/true) to include "stream":true.
  auto resp = chat(req);  // fallback to non-streaming until TODO above is done
  if (resp and cb) cb(resp->content);
  return resp;
}

}  // namespace jfsh
