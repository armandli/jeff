# libcurl C++ Examples

Every example builds on `curlx.hpp` from [cpp-wrappers.md](cpp-wrappers.md). Each was compiled with
`-std=c++23 -Wall -Wextra -Wconversion` and run under AddressSanitizer against libcurl 8.18.0 and a
local test server. JSON parsing is left to the caller; in this repo, use `jeff::json`
(simdjson-backed, see the simdjson-guide-cpp skill).

## Contents

1. [A reusable JSON client](#1-a-reusable-json-client)
2. [Streaming SSE (OpenAI / Anthropic style)](#2-streaming-sse-openai--anthropic-style)
3. [Streaming NDJSON (Ollama style)](#3-streaming-ndjson-ollama-style)
4. [Parallel requests with the multi interface](#4-parallel-requests-with-the-multi-interface)
5. [One handle per thread](#5-one-handle-per-thread)
6. [Download to a file](#6-download-to-a-file)
7. [Retry with backoff](#7-retry-with-backoff)
8. [GoogleTest against an in-process server](#8-googletest-against-an-in-process-server)

## 1. A reusable JSON client

One object owns one handle, so consecutive calls reuse the TCP/TLS connection. The object is not
thread-safe: use one per thread (§5).

```cpp
class JsonClient {
public:
  explicit JsonClient(std::string base_url, std::vector<std::string> headers = {})
    : base_(std::move(base_url)), h_(curlx::make_easy()) {
    curlx::append(hdrs_, "Content-Type: application/json");
    curlx::append(hdrs_, "Accept: application/json");
    curlx::append(hdrs_, "Expect:");
    for (const auto& line : headers) curlx::append(hdrs_, line);  // e.g. "x-api-key: …"
  }

  std::expected<std::string, curlx::Error> get(const std::string& path) {
    return run(path, nullptr);
  }
  std::expected<std::string, curlx::Error> post(const std::string& path, const std::string& body) {
    return run(path, &body);
  }

private:
  std::expected<std::string, curlx::Error> run(const std::string& path, const std::string* body) {
    CURL* h = h_.get();
    curl_easy_reset(h);  // options are sticky: start every request from defaults
    curlx::StringSink sink;
    std::string url = base_ + path;
    curlx::setopt<CURLOPT_URL>(h, url.c_str());
    curlx::setopt<CURLOPT_PROTOCOLS_STR>(h, "https,http");
    curlx::setopt<CURLOPT_HTTPHEADER>(h, hdrs_.get());  // member: lives as long as the handle
    curlx::setopt<CURLOPT_NOSIGNAL>(h, 1L);
    curlx::setopt<CURLOPT_CONNECTTIMEOUT_MS>(h, 5'000L);
    curlx::setopt<CURLOPT_TIMEOUT_MS>(h, 30'000L);
    curlx::setopt<CURLOPT_ACCEPT_ENCODING>(h, "");     // gzip/br/zstd, decoded for us
    curlx::setopt<CURLOPT_WRITEFUNCTION>(h, &curlx::StringSink::write);
    curlx::setopt<CURLOPT_WRITEDATA>(h, &sink);
    if (body) {
      curlx::setopt<CURLOPT_POSTFIELDS>(h, body->data());  // caller's string outlives perform
      curlx::setopt<CURLOPT_POSTFIELDSIZE_LARGE>(h, body->size());
    }
    auto r = curlx::perform(h, sink);
    if (not r) {
      if (not sink.body.empty()) r.error().message += ": " + sink.body.substr(0, 512);
      return std::unexpected(std::move(r.error()));
    }
    return std::move(sink.body);
  }

  std::string  base_;
  curlx::Slist hdrs_;  // declared before h_: destroyed after the handle that points to it
  curlx::Easy  h_;
};
```

Member order matters: members are destroyed in reverse order, so `hdrs_` must be declared before
`h_`. The handle is then cleaned up while the header list it points to is still alive.

## 2. Streaming SSE (OpenAI / Anthropic style)

Server-sent events are `field: value` lines, and a blank line ends an event. Chat APIs send JSON in
`data:` lines. OpenAI ends with `data: [DONE]`; Anthropic sends `event: message_stop`. Points to get
right:
- events split across chunks (`LineSink` buffers the partial line);
- **no total timeout**, but stall detection;
- cancellation;
- HTTP errors: a 4xx arrives as a plain JSON body, not as events.

```cpp
struct SseEvent { std::string event; std::string data; };

// on_event returns false to stop the stream early.
template <class OnEvent>
std::expected<void, curlx::Error>
stream_sse(CURL* h, const std::string& url, const std::string& body, curl_slist* headers,
           const std::atomic<bool>* cancel, OnEvent on_event) {
  SseEvent cur;
  std::string raw;  // everything received, kept for the error message on non-2xx
  auto on_line = [&](std::string_view line) -> bool {
    if (raw.size() < 4096) raw.append(line).push_back('\n');
    if (line.empty()) {                         // blank line: dispatch the event
      if (cur.data.empty() and cur.event.empty()) return true;
      if (not cur.data.empty() and cur.data.back() == '\n') cur.data.pop_back();
      bool go_on = on_event(cur);
      cur = {};
      return go_on;
    }
    if (line.front() == ':') return true;       // comment / keep-alive
    auto colon = line.find(':');
    std::string_view field = line.substr(0, colon);
    std::string_view value = colon == line.npos ? std::string_view{} : line.substr(colon + 1);
    if (not value.empty() and value.front() == ' ') value.remove_prefix(1);
    if (field == "data") cur.data.append(value).push_back('\n');
    else if (field == "event") cur.event = value;
    return true;
  };
  curlx::LineSink<decltype(on_line)> sink{on_line};

  curl_easy_reset(h);
  curlx::setopt<CURLOPT_URL>(h, url.c_str());
  curlx::setopt<CURLOPT_HTTPHEADER>(h, headers);  // include "Accept: text/event-stream"
  curlx::setopt<CURLOPT_POSTFIELDS>(h, body.data());
  curlx::setopt<CURLOPT_POSTFIELDSIZE_LARGE>(h, body.size());
  curlx::setopt<CURLOPT_WRITEFUNCTION>(h, &decltype(sink)::write);
  curlx::setopt<CURLOPT_WRITEDATA>(h, &sink);
  curlx::setopt<CURLOPT_NOSIGNAL>(h, 1L);
  curlx::setopt<CURLOPT_CONNECTTIMEOUT_MS>(h, 10'000L);
  curlx::setopt<CURLOPT_LOW_SPEED_LIMIT>(h, 1L);  // abort if ~nothing arrives...
  curlx::setopt<CURLOPT_LOW_SPEED_TIME>(h, 60L);  // ...for 60 s (fires ~66 s after the stall)
  curlx::setopt<CURLOPT_TCP_KEEPALIVE>(h, 1L);
  if (cancel) curlx::set_cancel_flag(h, cancel);  // stops within ~1 s

  auto r = curlx::perform(h, sink);
  if (not r) {
    r.error().message += ": " + raw;
    return std::unexpected(std::move(r.error()));
  }
  return {};
}
```

Usage:

```cpp
curlx::Slist hdrs;
curlx::append(hdrs, "Content-Type: application/json");
curlx::append(hdrs, "Accept: text/event-stream");
curlx::append(hdrs, "Authorization: Bearer " + key);
std::string text;
auto r = stream_sse(h, url, body, hdrs.get(), &cancel_flag, [&](const SseEvent& ev) {
  if (ev.data == "[DONE]" or ev.event == "message_stop") return false;  // finished
  text += extract_delta(ev.data);  // parse the JSON delta; may throw (rethrown by perform)
  return true;
});
```

For instant cancellation (Ctrl-C), run the same setup through `curlx::perform_interruptible` with
a `CURLM` that the signal-handling thread wakes. A signal handler itself may only set a
`std::atomic<bool>`. Call `curl_multi_wakeup` from a normal thread, or have the transfer loop poll
the flag through `curl_multi_poll`'s timeout.

## 3. Streaming NDJSON (Ollama style)

One JSON object per line; the last one has `"done": true`.

```cpp
template <class OnObject>  // bool(std::string_view json_line)
std::expected<void, curlx::Error>
stream_ndjson(CURL* h, const std::string& url, const std::string& body, OnObject on_object) {
  auto on_line = [&](std::string_view line) { return line.empty() or on_object(line); };
  curlx::LineSink<decltype(on_line)> sink{on_line};
  curlx::Slist hdrs;
  curlx::append(hdrs, "Content-Type: application/json");

  curl_easy_reset(h);
  curlx::setopt<CURLOPT_URL>(h, url.c_str());
  curlx::setopt<CURLOPT_HTTPHEADER>(h, hdrs.get());
  curlx::setopt<CURLOPT_POSTFIELDS>(h, body.data());
  curlx::setopt<CURLOPT_POSTFIELDSIZE_LARGE>(h, body.size());
  curlx::setopt<CURLOPT_WRITEFUNCTION>(h, &decltype(sink)::write);
  curlx::setopt<CURLOPT_WRITEDATA>(h, &sink);
  curlx::setopt<CURLOPT_CONNECTTIMEOUT_MS>(h, 2'000L);  // local server: fail fast
  curlx::setopt<CURLOPT_LOW_SPEED_LIMIT>(h, 1L);
  curlx::setopt<CURLOPT_LOW_SPEED_TIME>(h, 120L);       // model load can be slow
  auto r = curlx::perform(h, sink);
  curlx::setopt<CURLOPT_HTTPHEADER>(h, nullptr);        // hdrs dies here
  if (not r) return std::unexpected(std::move(r.error()));
  if (not sink.stopped and not sink.pending.empty()) on_object(sink.pending);  // last line, no '\n'
  return {};
}
```

## 4. Parallel requests with the multi interface

Fetch N URLs concurrently on one thread. Use `CURLOPT_PRIVATE` to map a finished easy handle back to
its slot.

```cpp
struct Fetch { std::string url; curlx::StringSink sink; CURLcode rc = CURLE_OK; long status = 0; };

void fetch_all(std::vector<Fetch>& jobs, long max_parallel = 8) {
  curlx::Multi m(curl_multi_init());
  if (not m) throw std::bad_alloc();
  curl_multi_setopt(m.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, max_parallel);
  std::vector<curlx::Easy> handles;  // declared after m: destroyed before it
  handles.reserve(jobs.size());
  for (std::size_t i = 0; i < jobs.size(); ++i) {
    curlx::Easy h = curlx::make_easy();
    curlx::setopt<CURLOPT_URL>(h.get(), jobs[i].url.c_str());
    curlx::setopt<CURLOPT_WRITEFUNCTION>(h.get(), &curlx::StringSink::write);
    curlx::setopt<CURLOPT_WRITEDATA>(h.get(), &jobs[i].sink);
    curlx::setopt<CURLOPT_PRIVATE>(h.get(), &jobs[i]);
    curlx::setopt<CURLOPT_TIMEOUT_MS>(h.get(), 30'000L);
    curl_multi_add_handle(m.get(), h.get());
    handles.push_back(std::move(h));
  }
  for (int running = 1; running;) {
    if (curl_multi_perform(m.get(), &running) != CURLM_OK) break;
    int queued = 0;
    while (CURLMsg* msg = curl_multi_info_read(m.get(), &queued)) {
      if (msg->msg != CURLMSG_DONE) continue;
      Fetch* job = nullptr;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &job);
      job->rc = msg->data.result;  // read before remove: msg dies with it
      curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &job->status);
      curl_multi_remove_handle(m.get(), msg->easy_handle);
    }
    if (running) curl_multi_poll(m.get(), nullptr, 0, 1000, nullptr);
  }
  for (auto& h : handles) curl_multi_remove_handle(m.get(), h.get());  // no-op if already removed
}
```

`jobs` must not reallocate while transfers run, because `WRITEDATA` and `PRIVATE` point into it.
Check each job's `rc` **and** `status`, and rethrow `sink.error` if it is set.

## 5. One handle per thread

Keep a handle per worker thread and reuse it, so each thread keeps its own warm connections.

```cpp
CURL* thread_handle() {
  thread_local curlx::Easy h = curlx::make_easy();
  return h.get();
}
```

- `curl_global_init` (`curlx::GlobalInit`) must have run in `main` before any worker starts.
- The `thread_local` is destroyed at thread exit; `curl_global_cleanup` must run after all workers
  have joined. Do not call `thread_handle()` on the main thread when `GlobalInit` is a local in
  `main`: the main thread's `thread_local` is destroyed after `main` returns, so after
  `curl_global_cleanup`.
- Every request still begins with `curl_easy_reset` (as in §1), because the previous request's
  pointers are dangling by then.

## 6. Download to a file

`CURLOPT_WRITEDATA` with the default write function takes a `FILE*`. Own it with RAII, and write to a
temporary name that is renamed only on success:

```cpp
std::expected<void, curlx::Error> download(CURL* h, const std::string& url, const std::string& path) {
  struct FileCloser { void operator()(std::FILE* f) const noexcept { std::fclose(f); } };
  std::string tmp = path + ".part";
  std::unique_ptr<std::FILE, FileCloser> f(std::fopen(tmp.c_str(), "wb"));
  if (not f) return std::unexpected(curlx::Error{CURLE_WRITE_ERROR, 0, "cannot open " + tmp});
  curl_easy_reset(h);
  curlx::setopt<CURLOPT_URL>(h, url.c_str());
  curlx::setopt<CURLOPT_PROTOCOLS_STR>(h, "https");
  curlx::setopt<CURLOPT_FOLLOWLOCATION>(h, 1L);
  curlx::setopt<CURLOPT_MAXREDIRS>(h, 5L);
  curlx::setopt<CURLOPT_REDIR_PROTOCOLS_STR>(h, "https");
  curlx::setopt<CURLOPT_WRITEDATA>(h, f.get());  // default WRITEFUNCTION is fwrite
  curlx::setopt<CURLOPT_LOW_SPEED_LIMIT>(h, 1024L);
  curlx::setopt<CURLOPT_LOW_SPEED_TIME>(h, 30L);
  auto r = curlx::perform(h);
  bool closed = std::fclose(f.release()) == 0;   // flush errors count as failures
  if (not r or not closed) {
    std::remove(tmp.c_str());
    if (not r) return std::unexpected(std::move(r.error()));
    return std::unexpected(curlx::Error{CURLE_WRITE_ERROR, 0, "write failed: " + tmp});
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0)
    return std::unexpected(curlx::Error{CURLE_WRITE_ERROR, 0, "rename failed: " + path});
  return {};
}
```

On Windows, the default `fwrite` callback crashes if libcurl and the program use different C
runtimes. Set an explicit write callback there.

## 7. Retry with backoff

Retry only what is safe: transport failures before any response, and 429/502/503/504. POSTs that
create something are not idempotent, so retry those only if the API documents an idempotency key.

```cpp
bool retryable(const curlx::Error& e) {
  switch (e.code) {
    case CURLE_COULDNT_RESOLVE_HOST: case CURLE_COULDNT_CONNECT: case CURLE_OPERATION_TIMEDOUT:
    case CURLE_GOT_NOTHING: case CURLE_SEND_ERROR: case CURLE_RECV_ERROR: case CURLE_PARTIAL_FILE:
      return true;
    case CURLE_OK:
      return e.http_status == 429 or e.http_status == 502 or e.http_status == 503 or e.http_status == 504;
    default:
      return false;  // TLS verification, bad URL, write errors, cancellation: retrying won't help
  }
}

template <class Call>  // std::expected<T, curlx::Error>()
auto with_retry(CURL* h, Call call, int attempts = 4) {
  auto delay = std::chrono::milliseconds(250);
  for (int i = 1;; ++i) {
    auto r = call();
    if (r or i == attempts or not retryable(r.error())) return r;
    curl_off_t retry_after = 0;  // seconds, from a Retry-After header
    curl_easy_getinfo(h, CURLINFO_RETRY_AFTER, &retry_after);
    std::this_thread::sleep_for(retry_after > 0 ? std::chrono::milliseconds(retry_after * 1000) : delay);
    delay *= 2;
  }
}
```

## 8. GoogleTest against an in-process server

cpp-httplib is already a dependency of this repo (jeff-server), so it can serve test responses from
inside the test process. The server binds an ephemeral port on 127.0.0.1, which avoids port
conflicts and needs no network.

```cpp
#include <httplib.h>
#include <gtest/gtest.h>

class LocalServer : public ::testing::Test {
protected:
  void SetUp() override {
    svr.Post("/v1/echo", [](const httplib::Request& req, httplib::Response& res) {
      res.set_content(req.body, "application/json");
    });
    svr.Get("/v1/stream", [](const httplib::Request&, httplib::Response& res) {
      res.set_chunked_content_provider("text/event-stream", [](size_t, httplib::DataSink& sink) {
        // one event split across two writes
        for (const char* part : {"data: {\"a\":", "1}\n\ndata: [DONE]\n\n"}) sink.write(part, std::strlen(part));
        sink.done();
        return true;
      });
    });
    port = svr.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    th = std::thread([this] { svr.listen_after_bind(); });
    svr.wait_until_ready();
  }
  void TearDown() override { svr.stop(); th.join(); }
  std::string url(const char* path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }

  httplib::Server svr;
  std::thread th;
  int port = 0;
};

TEST_F(LocalServer, StreamLinesSurviveChunkSplits) {
  std::vector<std::string> lines;
  auto on = [&](std::string_view l) { if (not l.empty()) lines.emplace_back(l); return true; };
  curlx::LineSink<decltype(on)> sink{on};
  curlx::Easy h = curlx::make_easy();
  curlx::setopt<CURLOPT_URL>(h.get(), url("/v1/stream").c_str());
  curlx::setopt<CURLOPT_WRITEFUNCTION>(h.get(), &decltype(sink)::write);
  curlx::setopt<CURLOPT_WRITEDATA>(h.get(), &sink);
  ASSERT_TRUE(curlx::perform(h.get(), sink));
  EXPECT_EQ(lines, (std::vector<std::string>{"data: {\"a\":1}", "data: [DONE]"}));
}

TEST_F(LocalServer, ConnectionRefusedIsTransportError) {
  curlx::Easy h = curlx::make_easy();
  curlx::setopt<CURLOPT_URL>(h.get(), "http://127.0.0.1:1/");
  auto r = curlx::perform(h.get());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, CURLE_COULDNT_CONNECT);
  EXPECT_EQ(r.error().http_status, 0);
}

int main(int argc, char** argv) {
  curlx::GlobalInit init;  // before any test thread starts
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
```

Also add these cases:
- a handler returning `res.status = 429` with a JSON error body (expect `http_status == 429` and
  the body in the message);
- a stalled stream, a chunked provider that sleeps past `LOW_SPEED_TIME` (use small values in
  tests);
- a cancel flag set from another thread.

Tests must never reach real provider APIs: keep keys out of CI and avoid flaky network.
