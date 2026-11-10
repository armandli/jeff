#include <jfsh/agent/openai.hh>
#include <curl/curl.h>
#include <cstring>
#include <sstream>

namespace jfsh {

// ---------------------------------------------------------------------------
// CURL write callback
// ---------------------------------------------------------------------------
static std::size_t curl_write(char* ptr, std::size_t, std::size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, nmemb);
    return nmemb;
}

struct StreamState {
    StreamCallback cb;
    std::string    content;
};

static std::size_t curl_stream_write(char* ptr, std::size_t, std::size_t nmemb, void* userdata) {
    auto* state = static_cast<StreamState*>(userdata);
    std::string chunk{ptr, nmemb};
    // Parse SSE lines
    std::istringstream ss{chunk};
    std::string line;
    while (std::getline(ss, line)) {
        if (line.substr(0, 6) == "data: ") {
            std::string data = line.substr(6);
            if (data == "[DONE]") continue;
            // Extract delta content — simple string search
            auto pos = data.find("\"content\":\"");
            if (pos != std::string::npos) {
                pos += 11;
                std::string text;
                for (; pos < data.size() && data[pos] != '"'; ++pos) {
                    if (data[pos] == '\\' && pos + 1 < data.size()) {
                        char esc = data[++pos];
                        if (esc == 'n') text += '\n';
                        else if (esc == 't') text += '\t';
                        else text += esc;
                    } else {
                        text += data[pos];
                    }
                }
                if (!text.empty()) {
                    state->content += text;
                    if (state->cb) state->cb(text);
                }
            }
        }
    }
    return nmemb;
}

// ---------------------------------------------------------------------------
// OpenAIBackend
// ---------------------------------------------------------------------------

OpenAIBackend::OpenAIBackend(ProviderConfig cfg) : cfg_(std::move(cfg)) {}

std::string OpenAIBackend::build_body(const ChatRequest& req) const {
    std::string body = "{\"model\":\"" + req.config.model + "\",";
    body += "\"stream\":" + std::string(req.stream ? "true" : "false") + ",";
    body += "\"max_tokens\":" + std::to_string(req.config.params.max_tokens) + ",";
    body += "\"messages\":[";
    for (std::size_t i = 0; i < req.messages.size(); ++i) {
        if (i) body += ",";
        const char* role = "user";
        if (req.messages[i].role == Role::System)    role = "system";
        if (req.messages[i].role == Role::Assistant) role = "assistant";
        body += "{\"role\":\"" + std::string(role) + "\",\"content\":\"";
        for (char c : req.messages[i].content) {
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

Result<std::string> OpenAIBackend::http_post(std::string_view url,
                                              std::string_view body,
                                              std::string_view extra_headers) const {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected(ShellError{ErrorKind::AgentError, "curl_easy_init failed"});

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!cfg_.api_key.empty()) {
        std::string auth = "Authorization: Bearer " + cfg_.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }
    if (!extra_headers.empty()) {
        // extra_headers is newline-separated
        std::string eh_str{extra_headers};
        std::istringstream ss{eh_str};
        std::string h;
        while (std::getline(ss, h))
            if (!h.empty()) headers = curl_slist_append(headers, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL,            std::string(url).c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     std::string(body).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,     static_cast<long>(cfg_.timeout_ms));

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        return std::unexpected(ShellError{ErrorKind::AgentError,
            std::string("curl: ") + curl_easy_strerror(rc)});
    return response;
}

Result<ChatResponse> OpenAIBackend::parse_response(std::string_view json) const {
    ChatResponse r;
    // Extract content from: "choices":[{"message":{"content":"..."}}]
    auto pos = json.find("\"content\":\"");
    if (pos == std::string_view::npos)
        return std::unexpected(ShellError{ErrorKind::AgentError, "no content in response"});
    pos += 11;
    for (; pos < json.size() && json[pos] != '"'; ++pos) {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char esc = json[++pos];
            if (esc == 'n') r.content += '\n';
            else if (esc == 't') r.content += '\t';
            else r.content += esc;
        } else {
            r.content += json[pos];
        }
    }
    return r;
}

Result<ChatResponse> OpenAIBackend::chat(const ChatRequest& req) {
    std::string url = (cfg_.base_url.empty() ? "https://api.openai.com/v1" : cfg_.base_url)
                      + "/chat/completions";
    auto body = build_body(req);
    auto resp = http_post(url, body);
    if (!resp) return std::unexpected(resp.error());
    return parse_response(*resp);
}

Result<ChatResponse> OpenAIBackend::chat_stream(const ChatRequest& req, StreamCallback cb) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected(ShellError{ErrorKind::AgentError, "curl_easy_init failed"});

    std::string url = (cfg_.base_url.empty() ? "https://api.openai.com/v1" : cfg_.base_url)
                      + "/chat/completions";
    // Force stream=true
    ChatRequest sreq = req;
    sreq.stream = true;
    std::string body = build_body(sreq);

    StreamState state{cb, {}};
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    if (!cfg_.api_key.empty()) {
        std::string auth = "Authorization: Bearer " + cfg_.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_stream_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &state);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,    static_cast<long>(cfg_.timeout_ms));

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        return std::unexpected(ShellError{ErrorKind::AgentError,
            std::string("curl: ") + curl_easy_strerror(rc)});

    ChatResponse r;
    r.content = std::move(state.content);
    return r;
}

std::optional<std::string> OpenAIBackend::parse_sse_chunk(std::string_view /*line*/) const {
    return std::nullopt;
}

} // namespace jfsh
