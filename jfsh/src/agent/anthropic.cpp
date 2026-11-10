#include <jfsh/agent/anthropic.hh>
#include <curl/curl.h>
#include <sstream>

namespace jfsh {

static std::size_t curl_write_anthropic(char* ptr, std::size_t, std::size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, nmemb);
    return nmemb;
}

AnthropicBackend::AnthropicBackend(ProviderConfig cfg) : cfg_(std::move(cfg)) {}

std::string AnthropicBackend::build_body(const ChatRequest& req) const {
    std::string system_text;
    std::string body = "{\"model\":\"" + req.config.model + "\",";
    body += "\"max_tokens\":" + std::to_string(req.config.params.max_tokens) + ",";
    // Extract system message
    std::vector<const Message*> chat_msgs;
    for (const auto& m : req.messages) {
        if (m.role == Role::System) system_text = m.content;
        else chat_msgs.push_back(&m);
    }
    if (!system_text.empty()) {
        body += "\"system\":\"";
        for (char c : system_text) {
            if (c == '"') body += "\\\"";
            else if (c == '\\') body += "\\\\";
            else if (c == '\n') body += "\\n";
            else body += c;
        }
        body += "\",";
    }
    if (req.stream) body += "\"stream\":true,";
    body += "\"messages\":[";
    for (std::size_t i = 0; i < chat_msgs.size(); ++i) {
        if (i) body += ",";
        const char* role = chat_msgs[i]->role == Role::Assistant ? "assistant" : "user";
        body += "{\"role\":\"" + std::string(role) + "\",\"content\":\"";
        for (char c : chat_msgs[i]->content) {
            if (c == '"') body += "\\\"";
            else if (c == '\\') body += "\\\\";
            else if (c == '\n') body += "\\n";
            else body += c;
        }
        body += "\"}";
    }
    body += "]}";
    return body;
}

Result<std::string> AnthropicBackend::http_post(std::string_view url,
                                                 std::string_view body) const {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected(ShellError{ErrorKind::AgentError, "curl init failed"});

    std::string resp;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    std::string ak = "x-api-key: " + cfg_.api_key;
    hdrs = curl_slist_append(hdrs, ak.c_str());
    std::string av = std::string("anthropic-version: ") + std::string(kApiVersion);
    hdrs = curl_slist_append(hdrs, av.c_str());

    curl_easy_setopt(curl, CURLOPT_URL,           std::string(url).c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    std::string(body).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_anthropic);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    static_cast<long>(cfg_.timeout_ms));

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        return std::unexpected(ShellError{ErrorKind::AgentError,
            std::string("curl: ") + curl_easy_strerror(rc)});
    return resp;
}

Result<ChatResponse> AnthropicBackend::parse_response(std::string_view json) const {
    ChatResponse r;
    // Anthropic response: {"content":[{"type":"text","text":"..."}]}
    auto pos = json.find("\"text\":\"");
    if (pos == std::string_view::npos)
        return std::unexpected(ShellError{ErrorKind::AgentError, "no text in response"});
    pos += 8;
    for (; pos < json.size() && json[pos] != '"'; ++pos) {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char esc = json[++pos];
            if (esc == 'n') r.content += '\n';
            else r.content += esc;
        } else {
            r.content += json[pos];
        }
    }
    return r;
}

std::optional<std::string> AnthropicBackend::parse_sse_chunk(std::string_view /*line*/) const {
    return std::nullopt;
}

Result<ChatResponse> AnthropicBackend::chat(const ChatRequest& req) {
    std::string url = (cfg_.base_url.empty() ? "https://api.anthropic.com" : cfg_.base_url)
                      + "/v1/messages";
    auto body = build_body(req);
    auto resp = http_post(url, body);
    if (!resp) return std::unexpected(resp.error());
    return parse_response(*resp);
}

Result<ChatResponse> AnthropicBackend::chat_stream(const ChatRequest& req, StreamCallback cb) {
    // TODO(anthropic/stream): Implement SSE streaming for the Anthropic API.
    //   Anthropic SSE events:
    //     event: content_block_delta  → data: {"type":"content_block_delta","delta":{"text":"..."}}
    //     event: message_stop         → streaming complete
    //   Use a CURLOPT_WRITEFUNCTION callback (like curl_stream_write in openai.cpp)
    //   but parse the anthropic-specific event structure.
    //   Wire CURLOPT_HTTPHEADER with Accept: text/event-stream.
    ChatRequest sreq = req; sreq.stream = true;
    auto resp = chat(sreq);  // fallback to non-streaming until TODO above is done
    if (resp && cb) cb(resp->content);
    return resp;
}

} // namespace jfsh
