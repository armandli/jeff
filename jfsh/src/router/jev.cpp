#include <jfsh/router/jev.h>

#include <cstdio>
#include <cstdlib>

#include <curl/curl.h>
#include <json.h>

#include <jfsh/core/aliases.h>

namespace jfsh {

static s::size_t curl_write_jev(char* ptr, s::size_t, s::size_t nmemb,
                                 void* ud) {
  static_cast<str*>(ud)->append(ptr, nmemb);
  return nmemb;
}

JevConfig JevConfig::from_env() {
  JevConfig cfg;
  const char* key = s::getenv("TYPESAFE_API_KEY");
  if (key) cfg.api_key = key;
  return cfg;
}

str JevRouter::build_body(strv line) const {
  jeff::json::Writer w;
  w.begin_object()
      .field("model", mCfg.model)
      .field("state", line)
      .key("questions").begin_object()
        .key("routing").begin_object()
          .field("type", "choice")
          .key("criteria").begin_object()
            .field("run",   "Execute as a shell command or script.")
            .field("agent", "Answer as a natural-language question or task "
                            "with an AI assistant.")
          .end_object()
        .end_object()
      .end_object()
    .end_object();
  return w.str();
}

Result<str> JevRouter::http_post(strv url,
                                  strv body) const {
  CURL* curl = curl_easy_init();
  if (not curl)
    return s::unexpected(ShellError{ErrorKind::RouterError, "curl init"});

  str resp;
  struct curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  str auth = "Authorization: Bearer " + mCfg.api_key;
  hdrs = curl_slist_append(hdrs, auth.c_str());

  curl_easy_setopt(curl, CURLOPT_URL,           str(url).c_str());
  // POSTFIELDS is not copied by curl; body outlives curl_easy_perform below.
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.data());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_jev);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(mCfg.timeout_ms));

  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK)
    return s::unexpected(ShellError{ErrorKind::RouterError,
        str("curl: ") + curl_easy_strerror(rc)});
  return resp;
}

Result<RouterDecision> JevRouter::parse_response(
    strv json) const {
  // {"answers":{"routing":{"choice":"run"|"agent","confidence":<float>}}}
  auto parsed = jeff::json::parse(json);
  if (not parsed)
    return s::unexpected(ShellError{ErrorKind::RouterError,
        "jev: invalid response: " + parsed.error});
  const jeff::json::Value* routing = parsed.value.at({"answers", "routing"});
  const jeff::json::Value* choice  = routing ? routing->find("choice") : nullptr;
  if (not choice or not choice->is_string())
    return s::unexpected(
        ShellError{ErrorKind::RouterError, "jev: no choice in response"});

  // Confidence may sit beside the choice or at the top level.
  double confidence = parsed.value.get_double("confidence", 0.9);
  confidence        = routing->get_double("confidence", confidence);

  RouterDecision d;
  d.model_name = "jev";
  d.verdict    = (*choice->as_string() == "agent") ? RouterVerdict::Agent
                                                   : RouterVerdict::Run;
  d.confidence = static_cast<float>(confidence);
  return d;
}

RouterDecision JevRouter::classify(strv line) {
  str  url  = mCfg.base_url + "/v1/systemone";
  auto body = build_body(line);
  auto resp = http_post(url, body);
  if (not resp) {
    s::fprintf(stderr,
               "jfsh: jev router: %s — falling back to heuristic\n",
               resp.error().message.c_str());
    HeuristicRouter fallback;
    return fallback.classify(line);
  }
  auto decision = parse_response(*resp);
  if (not decision) {
    s::fprintf(stderr,
               "jfsh: jev router: %s — falling back to heuristic\n",
               decision.error().message.c_str());
    HeuristicRouter fallback;
    return fallback.classify(line);
  }
  return *decision;
}

}  // namespace jfsh
