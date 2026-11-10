#include <cstdio>

#include <curl/curl.h>

#include <jfsh/router/jeff.hh>

namespace jfsh {

static std::size_t curl_write_jeff(char* ptr, std::size_t, std::size_t nmemb,
                                    void* ud) {
  static_cast<std::string*>(ud)->append(ptr, nmemb);
  return nmemb;
}

JeffRouter::JeffRouter(JeffConfig cfg) : mCfg(std::move(cfg)) {}

std::string JeffRouter::build_body(std::string_view line) const {
  std::string body = "{\"line\":\"";
  for (char c : line) {
    if      (c == '"')  body += "\\\"";
    else if (c == '\\') body += "\\\\";
    else if (c == '\n') body += "\\n";
    else                body += c;
  }
  body += "\"}";
  return body;
}

Result<std::string> JeffRouter::http_post(std::string_view url,
                                           std::string_view body) const {
  CURL* curl = curl_easy_init();
  if (not curl)
    return std::unexpected(ShellError{ErrorKind::RouterError, "curl init"});

  std::string resp;
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL,           std::string(url).c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    std::string(body).c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_jeff);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(mCfg.timeout_ms));

  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    return std::unexpected(ShellError{ErrorKind::RouterError,
        std::string("curl: ") + curl_easy_strerror(rc)});
  return resp;
}

Result<RouterDecision> JeffRouter::parse_response(
    std::string_view json) const {
  RouterDecision d;
  d.model_name = "jeff";

  // Extract verdict from: {"verdict":"run"|"agent","confidence":<float>}
  auto vpos = json.find("\"verdict\":\"");
  if (vpos == std::string_view::npos)
    return std::unexpected(
        ShellError{ErrorKind::RouterError, "jeff: no verdict in response"});
  vpos += 11;
  std::string verdict;
  for (; vpos < json.size() and json[vpos] != '"'; ++vpos)
    verdict += json[vpos];

  auto cpos = json.find("\"confidence\":");
  float confidence = 0.9f;
  if (cpos != std::string_view::npos) {
    cpos += 13;
    std::string cs;
    for (; cpos < json.size() and
           (json[cpos] == '.' or
            static_cast<bool>(std::isdigit(json[cpos]))); ++cpos)
      cs += json[cpos];
    if (not cs.empty()) confidence = std::stof(cs);
  }

  d.verdict    = (verdict == "agent") ? RouterVerdict::Agent
                                      : RouterVerdict::Run;
  d.confidence = confidence;
  return d;
}

RouterDecision JeffRouter::classify(std::string_view line) {
  std::string url  = mCfg.base_url + "/classify";
  auto        body = build_body(line);
  auto        resp = http_post(url, body);
  if (not resp) {
    std::fprintf(stderr,
                 "jfsh: jeff router: %s — falling back to heuristic\n",
                 resp.error().message.c_str());
    HeuristicRouter fallback;
    return fallback.classify(line);
  }
  auto decision = parse_response(*resp);
  if (not decision) {
    std::fprintf(stderr,
                 "jfsh: jeff router: %s — falling back to heuristic\n",
                 decision.error().message.c_str());
    HeuristicRouter fallback;
    return fallback.classify(line);
  }
  return *decision;
}

}  // namespace jfsh
