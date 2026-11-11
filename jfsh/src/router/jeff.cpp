#include <cstdio>

#include <curl/curl.h>

#include <json.hpp>

#include <jfsh/router/jeff.hh>

namespace jfsh {

static std::size_t curl_write_jeff(char* ptr, std::size_t, std::size_t nmemb,
                                    void* ud) {
  static_cast<std::string*>(ud)->append(ptr, nmemb);
  return nmemb;
}

JeffRouter::JeffRouter(JeffConfig cfg) : mCfg(std::move(cfg)) {}

std::string JeffRouter::build_body(std::string_view line) const {
  jeff::json::Writer w;
  w.begin_object().field("line", line).end_object();
  return w.str();
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
  // POSTFIELDS is not copied by curl; body outlives curl_easy_perform below.
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.data());
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
  // {"verdict":"run"|"agent","confidence":<float>,"model":"jeff"}
  auto parsed = jeff::json::parse(json);
  if (not parsed)
    return std::unexpected(ShellError{ErrorKind::RouterError,
        "jeff: invalid response: " + parsed.error});
  const jeff::json::Value* verdict = parsed.value.find("verdict");
  if (not verdict or not verdict->is_string())
    return std::unexpected(
        ShellError{ErrorKind::RouterError, "jeff: no verdict in response"});

  RouterDecision d;
  d.model_name = "jeff";
  d.verdict    = (*verdict->as_string() == "agent") ? RouterVerdict::Agent
                                                    : RouterVerdict::Run;
  d.confidence = static_cast<float>(parsed.value.get_double("confidence", 0.9));
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
