#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <curl/curl.h>

#include <jfsh/router/jev.hh>

namespace jfsh {

static std::size_t curl_write_jev(char* ptr, std::size_t, std::size_t nmemb,
                                   void* ud) {
  static_cast<std::string*>(ud)->append(ptr, nmemb);
  return nmemb;
}

JevConfig JevConfig::from_env() {
  JevConfig cfg;
  const char* key = std::getenv("TYPESAFE_API_KEY");
  if (key) cfg.api_key = key;
  return cfg;
}

JevRouter::JevRouter(JevConfig cfg) : mCfg(std::move(cfg)) {}

std::string JevRouter::build_body(std::string_view line) const {
  std::string body = "{\"model\":\"" + mCfg.model + "\",\"state\":\"";
  for (char c : line) {
    if      (c == '"')  body += "\\\"";
    else if (c == '\\') body += "\\\\";
    else if (c == '\n') body += "\\n";
    else                body += c;
  }
  body +=
    "\",\"questions\":{\"routing\":{"
    "\"type\":\"choice\","
    "\"criteria\":{"
    "\"run\":\"Execute as a shell command or script.\","
    "\"agent\":\"Answer as a natural-language question or task "
    "with an AI assistant.\"}}}}";
  return body;
}

Result<std::string> JevRouter::http_post(std::string_view url,
                                          std::string_view body) const {
  CURL* curl = curl_easy_init();
  if (not curl)
    return std::unexpected(ShellError{ErrorKind::RouterError, "curl init"});

  std::string resp;
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  std::string auth = "Authorization: Bearer " + mCfg.api_key;
  hdrs = curl_slist_append(hdrs, auth.c_str());

  curl_easy_setopt(curl, CURLOPT_URL,           std::string(url).c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    std::string(body).c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_jev);
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

Result<RouterDecision> JevRouter::parse_response(
    std::string_view json) const {
  RouterDecision d;
  d.model_name = "jev";

  // Extract the choice value from "answers":{"routing":{"choice":"<val>"}}
  auto choice_pos = json.find("\"choice\":\"");
  if (choice_pos == std::string_view::npos)
    return std::unexpected(
        ShellError{ErrorKind::RouterError, "jev: no choice in response"});
  choice_pos += 10;
  std::string choice;
  for (; choice_pos < json.size() and json[choice_pos] != '"'; ++choice_pos)
    choice += json[choice_pos];

  auto conf_pos = json.find("\"confidence\":");
  float confidence = 0.9f;
  if (conf_pos != std::string_view::npos) {
    conf_pos += 13;
    std::string cs;
    for (; conf_pos < json.size() and
           (json[conf_pos] == '.' or
            static_cast<bool>(std::isdigit(json[conf_pos]))); ++conf_pos)
      cs += json[conf_pos];
    if (not cs.empty()) confidence = std::stof(cs);
  }

  d.verdict    = (choice == "agent") ? RouterVerdict::Agent
                                     : RouterVerdict::Run;
  d.confidence = confidence;
  return d;
}

RouterDecision JevRouter::classify(std::string_view line) {
  std::string url  = mCfg.base_url + "/v1/systemone";
  auto        body = build_body(line);
  auto        resp = http_post(url, body);
  if (not resp) {
    std::fprintf(stderr,
                 "jfsh: jev router: %s — falling back to heuristic\n",
                 resp.error().message.c_str());
    HeuristicRouter fallback;
    return fallback.classify(line);
  }
  auto decision = parse_response(*resp);
  if (not decision) {
    std::fprintf(stderr,
                 "jfsh: jev router: %s — falling back to heuristic\n",
                 decision.error().message.c_str());
    HeuristicRouter fallback;
    return fallback.classify(line);
  }
  return *decision;
}

}  // namespace jfsh
