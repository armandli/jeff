#include <jfsh/agent/ollama.hh>
#include <curl/curl.h>
#include <sstream>

namespace jfsh {

static std::size_t curl_write_ollama(char* ptr, std::size_t, std::size_t nmemb, void* ud) {
    static_cast<std::string*>(ud)->append(ptr, nmemb);
    return nmemb;
}

OllamaBackend::OllamaBackend(ProviderConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.base_url.empty()) cfg_.base_url = "http://localhost:11434";
}

bool OllamaBackend::is_server_available() const noexcept {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string url = cfg_.base_url + "/api/tags";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

std::string OllamaBackend::build_body(const ChatRequest& req, bool stream) const {
    std::string body = "{\"model\":\"" + req.config.model + "\",";
    body += "\"stream\":" + std::string(stream ? "true" : "false") + ",";
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

Result<std::string> OllamaBackend::http_post(std::string_view url,
                                              std::string_view body) const {
    CURL* curl = curl_easy_init();
    if (!curl) return std::unexpected(ShellError{ErrorKind::AgentError, "curl init"});
    std::string resp;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,           std::string(url).c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    std::string(body).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_ollama);
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

Result<ChatResponse> OllamaBackend::parse_response(std::string_view json) const {
    ChatResponse r;
    // Ollama non-streaming: {"message":{"role":"assistant","content":"..."}}
    auto pos = json.find("\"content\":\"");
    if (pos == std::string_view::npos)
        return std::unexpected(ShellError{ErrorKind::AgentError, "no content"});
    pos += 11;
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

std::optional<std::string> OllamaBackend::parse_ndjson_chunk(std::string_view /*line*/) const {
    return std::nullopt;
}

Result<ChatResponse> OllamaBackend::chat(const ChatRequest& req) {
    std::string url = cfg_.base_url + "/api/chat";
    auto body = build_body(req, false);
    auto resp = http_post(url, body);
    if (!resp) return std::unexpected(resp.error());
    return parse_response(*resp);
}

Result<ChatResponse> OllamaBackend::chat_stream(const ChatRequest& req, StreamCallback cb) {
    // Streaming NDJSON — fall back to non-streaming for now
    auto resp = chat(req);
    if (resp && cb) cb(resp->content);
    return resp;
}

} // namespace jfsh
