# C++ Wrappers for libcurl

`curlx.hpp` below is a complete, tested header. It needs C++23 for `std::expected`. Copy it into a
project and adapt the namespace.

It was compiled with `-Wall -Wextra -Wconversion` and run under AddressSanitizer/UBSan against
libcurl 8.18.0 and a local server. The run covered:
- 404 handling;
- size caps;
- exception rethrow;
- SSE split across chunks;
- total and low-speed timeouts;
- both cancellation methods;
- connection reuse;
- URL encoding;
- the multi interface.

## Contents

- [Design choices](#design-choices)
- [curlx.hpp](#curlxhpp)
- [Using a class member as the callback](#using-a-class-member-as-the-callback)
- [Mapping CURLcode to std::error_code](#mapping-curlcode-to-stderror_code)
- [Without C++23](#without-c23)

## Design choices

| Gap in the C API | What the wrapper does |
|---|---|
| Handles must be freed on every path | `std::unique_ptr` aliases with stateless deleters: `Easy`, `Multi`, `Slist`, `Url` and `CurlStr`. They cost no more space than a raw pointer. |
| `curl_slist_append` returns NULL and keeps the list | `append(Slist&, …)` throws `std::bad_alloc` and leaves the list owned. |
| `curl_easy_setopt` is variadic and unchecked in C++ | `setopt<CURLOPT_X>(h, v)` takes the option as a template argument. Each `CURLoption` value encodes its type: `CURLOPTTYPE_LONG` 0, `OBJECTPOINT` 10000, `FUNCTIONPOINT` 20000, `OFF_T` 30000, `BLOB` 40000. A `static_assert` rejects `std::string`, `string_view`, non-integers for long options, and lambdas for function options. Integers are converted to the exact type libcurl reads. |
| Callbacks are C function pointers with a `void*` | Each sink is a struct holding its state and a `static` `noexcept` callback. `WRITEDATA` points at the struct. |
| Exceptions cannot cross libcurl | Sinks catch everything into `std::exception_ptr error` and return `CURL_WRITEFUNC_ERROR`. `perform(h, sink)` rethrows once `curl_easy_perform` has returned. |
| HTTP 4xx/5xx are `CURLE_OK` | `perform` reads `CURLINFO_RESPONSE_CODE` and returns `std::unexpected(Error{…, http_status})` for non-2xx. |
| `curl_easy_strerror` is generic | `perform` installs a 256-byte (`CURL_ERROR_SIZE`) error buffer on the stack for the call and removes it before returning, so the handle never points at a dead buffer. |
| An early stop from a write callback looks like an error | `LineSink::stopped` is set when the line handler returns false. `perform(h, sink)` turns that `CURLE_WRITE_ERROR` back into success. |
| Unbounded responses | `StringSink::limit` (64 MiB default) aborts the transfer with `CURLE_WRITE_ERROR`. |
| Query strings built by concatenation | `build_url` uses `CURLU` with `CURLU_APPENDQUERY \| CURLU_URLENCODE`. The output uses upper-case hex, e.g. `c%2B%2B`. |
| `curl_global_init` must run first, once | A `GlobalInit` object at the top of `main`. Not a static global: initialization order across translation units is unspecified, and the docs forbid calling it from static initializers on Windows. |

Things the wrapper deliberately does **not** do:
- **Hide `CURL*`.** Every libcurl option stays reachable, and the docs at https://curl.se/libcurl/c/
  map one-to-one onto the code.
- **Throw on transfer failure.** Network errors are expected, so they come back as `std::expected`.
  Exceptions are reserved for bugs and allocation failure.
- **Make a handle thread-safe.** As in C, use one handle per thread at a time.

## curlx.hpp

```cpp
#pragma once
// curlx: a thin C++23 layer over libcurl's C API. Ownership, type-checked setopt,
// exception-safe callbacks and std::expected errors. Copy into a project and adapt.
#include <curl/curl.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace curlx {

// ---- process-wide init -------------------------------------------------------
// Construct once at the top of main(), before any thread starts.
class GlobalInit {
public:
  GlobalInit() {
    if (CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT); rc != CURLE_OK)
      throw std::runtime_error(std::string("curl_global_init: ") + curl_easy_strerror(rc));
  }
  ~GlobalInit() { curl_global_cleanup(); }
  GlobalInit(const GlobalInit&) = delete;
  GlobalInit& operator=(const GlobalInit&) = delete;
};

// ---- owning handles ----------------------------------------------------------
struct EasyDeleter  { void operator()(CURL* p) const noexcept { curl_easy_cleanup(p); } };
struct MultiDeleter { void operator()(CURLM* p) const noexcept { curl_multi_cleanup(p); } };
struct SlistDeleter { void operator()(curl_slist* p) const noexcept { curl_slist_free_all(p); } };
struct UrlDeleter   { void operator()(CURLU* p) const noexcept { curl_url_cleanup(p); } };
struct FreeDeleter  { void operator()(char* p) const noexcept { curl_free(p); } };

using Easy    = std::unique_ptr<CURL, EasyDeleter>;
using Multi   = std::unique_ptr<CURLM, MultiDeleter>;
using Slist   = std::unique_ptr<curl_slist, SlistDeleter>;
using Url     = std::unique_ptr<CURLU, UrlDeleter>;
using CurlStr = std::unique_ptr<char, FreeDeleter>;  // strings from curl_url_get, curl_easy_escape

inline Easy make_easy() {
  Easy h(curl_easy_init());
  if (not h) throw std::bad_alloc();
  return h;
}

// curl_slist_append returns NULL on failure and leaves the old list intact;
// assigning that NULL straight back leaks the list.
inline void append(Slist& list, const char* line) {
  curl_slist* grown = curl_slist_append(list.get(), line);
  if (not grown) throw std::bad_alloc();
  (void)list.release();
  list.reset(grown);
}
inline void append(Slist& list, const std::string& line) { append(list, line.c_str()); }

// ---- type-checked setopt -----------------------------------------------------
// curl_easy_setopt is variadic, and curl's compile-time type checker is disabled for C++
// (curl.h: "the typechecker does not work in C++"). The option number encodes its type,
// so check it at compile time.
template <CURLoption Opt, class T>
CURLcode setopt(CURL* h, T&& value) {
  using V = std::remove_cvref_t<T>;
  using D = std::decay_t<T>;  // arrays -> pointers, function names -> function pointers
  constexpr int kind = static_cast<int>(Opt) / 10000 * 10000;
  if constexpr (kind == CURLOPTTYPE_LONG) {
    static_assert(std::is_integral_v<V> or std::is_enum_v<V>, "this option takes a long");
    return curl_easy_setopt(h, Opt, static_cast<long>(value));
  } else if constexpr (kind == CURLOPTTYPE_OFF_T) {
    static_assert(std::is_integral_v<V>, "this option takes a curl_off_t");
    return curl_easy_setopt(h, Opt, static_cast<curl_off_t>(value));
  } else if constexpr (kind == CURLOPTTYPE_FUNCTIONPOINT) {
    static_assert(std::is_pointer_v<D> and std::is_function_v<std::remove_pointer_t<D>>,
                  "pass a function pointer (use +[](...){} for a captureless lambda)");
    return curl_easy_setopt(h, Opt, static_cast<D>(value));
  } else if constexpr (kind == CURLOPTTYPE_OBJECTPOINT) {
    static_assert(not std::is_same_v<V, std::string> and not std::is_same_v<V, std::string_view>,
                  "pass .c_str() (string_view is not null-terminated)");
    static_assert(std::is_pointer_v<D> or std::is_null_pointer_v<D>, "this option takes a pointer");
    return curl_easy_setopt(h, Opt, static_cast<D>(value));
  } else {
    static_assert(std::is_same_v<V, curl_blob*>, "this option takes a curl_blob*");
    return curl_easy_setopt(h, Opt, value);
  }
}

// ---- errors ------------------------------------------------------------------
struct Error {
  CURLcode    code = CURLE_OK;
  long        http_status = 0;  // set when the transfer worked but the status was not 2xx
  std::string message;
};

// ---- write sinks -------------------------------------------------------------
// Callbacks must not throw through libcurl's C frames: catch, stash, return an error code,
// rethrow after curl_easy_perform returns.
struct StringSink {
  std::string        body;
  std::size_t        limit = 64u << 20;  // cap what a server can make us buffer
  std::exception_ptr error;

  static std::size_t write(char* ptr, std::size_t, std::size_t n, void* ud) noexcept {
    auto* self = static_cast<StringSink*>(ud);
    try {
      if (self->body.size() + n > self->limit) return CURL_WRITEFUNC_ERROR;
      self->body.append(ptr, n);
      return n;
    } catch (...) {
      self->error = std::current_exception();
      return CURL_WRITEFUNC_ERROR;
    }
  }
};

// Splits a byte stream into '\n'-terminated lines (NDJSON, SSE). Chunks arrive at arbitrary
// boundaries, so keep the partial tail between calls.
template <class OnLine>  // bool(std::string_view line): false stops the transfer
struct LineSink {
  OnLine             on_line;
  std::string        pending{};
  std::exception_ptr error{};
  bool               stopped = false;

  static std::size_t write(char* ptr, std::size_t, std::size_t n, void* ud) noexcept {
    auto* self = static_cast<LineSink*>(ud);
    try {
      self->pending.append(ptr, n);
      std::size_t start = 0;
      for (std::size_t nl; (nl = self->pending.find('\n', start)) != std::string::npos; start = nl + 1) {
        std::string_view line(self->pending.data() + start, nl - start);
        if (not line.empty() and line.back() == '\r') line.remove_suffix(1);
        if (not self->on_line(line)) { self->stopped = true; return CURL_WRITEFUNC_ERROR; }
      }
      self->pending.erase(0, start);
      return n;
    } catch (...) {
      self->error = std::current_exception();
      return CURL_WRITEFUNC_ERROR;
    }
  }
};

// ---- cancellation ------------------------------------------------------------
// Another thread sets the flag; libcurl polls it from the progress callback.
inline int cancel_progress(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
  return static_cast<const std::atomic<bool>*>(ud)->load(std::memory_order_relaxed) ? 1 : 0;
}
inline void set_cancel_flag(CURL* h, const std::atomic<bool>* flag) {
  setopt<CURLOPT_XFERINFOFUNCTION>(h, &cancel_progress);
  setopt<CURLOPT_XFERINFODATA>(h, const_cast<std::atomic<bool>*>(flag));
  setopt<CURLOPT_NOPROGRESS>(h, 0L);
}

// ---- perform -----------------------------------------------------------------
// Runs the transfer and turns every failure into an Error: transport errors and non-2xx
// HTTP status. libcurl itself reports a 404 or 500 as CURLE_OK.
inline std::expected<long, Error> perform(CURL* h) {
  char errbuf[CURL_ERROR_SIZE] = {};
  setopt<CURLOPT_ERRORBUFFER>(h, errbuf);
  CURLcode rc = curl_easy_perform(h);
  setopt<CURLOPT_ERRORBUFFER>(h, nullptr);  // errbuf dies with this frame
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  if (rc != CURLE_OK)
    return std::unexpected(Error{rc, status, errbuf[0] ? errbuf : curl_easy_strerror(rc)});
  if (status < 200 or status > 299)
    return std::unexpected(Error{rc, status, "HTTP " + std::to_string(status)});
  return status;
}

// Same, for a sink with an `error` member (rethrown here, outside libcurl) and optionally a
// `stopped` member (a deliberate early stop surfaces as CURLE_WRITE_ERROR; report success).
template <class Sink>
std::expected<long, Error> perform(CURL* h, Sink& sink) {
  auto r = perform(h);
  if (sink.error) std::rethrow_exception(std::exchange(sink.error, nullptr));
  if constexpr (requires { sink.stopped; })
    if (not r and sink.stopped and r.error().code == CURLE_WRITE_ERROR) return r.error().http_status;
  return r;
}

// ---- URL building ------------------------------------------------------------
// Build URLs with CURLU instead of string concatenation, so query values are encoded.
inline std::string build_url(const std::string& base, std::initializer_list<std::pair<std::string, std::string>> query) {
  Url u(curl_url());
  if (not u) throw std::bad_alloc();
  if (CURLUcode rc = curl_url_set(u.get(), CURLUPART_URL, base.c_str(), 0); rc != CURLUE_OK)
    throw std::invalid_argument(std::string("bad URL: ") + curl_url_strerror(rc));
  for (const auto& [k, v] : query) {
    std::string kv = k + "=" + v;  // URLENCODE on a query part encodes the value after the '='
    curl_url_set(u.get(), CURLUPART_QUERY, kv.c_str(), CURLU_APPENDQUERY | CURLU_URLENCODE);
  }
  char* out = nullptr;
  if (CURLUcode rc = curl_url_get(u.get(), CURLUPART_URL, &out, 0); rc != CURLUE_OK)
    throw std::invalid_argument(std::string("bad URL: ") + curl_url_strerror(rc));
  return std::string(CurlStr(out).get());
}

// ---- interruptible transfer ----------------------------------------------------
// Runs one easy transfer on a multi handle. Another thread stops it at once with
//   cancel.store(true); curl_multi_wakeup(m);
// Returns the transfer's CURLcode, or CURLE_ABORTED_BY_CALLBACK when cancelled.
inline CURLcode perform_interruptible(CURL* h, CURLM* m, const std::atomic<bool>& cancel) {
  if (curl_multi_add_handle(m, h) != CURLM_OK) return CURLE_FAILED_INIT;
  CURLcode result = CURLE_ABORTED_BY_CALLBACK;
  for (int running = 1; running and not cancel.load();) {
    if (curl_multi_perform(m, &running) != CURLM_OK) { result = CURLE_FAILED_INIT; break; }
    int queued = 0;
    while (CURLMsg* msg = curl_multi_info_read(m, &queued))
      if (msg->msg == CURLMSG_DONE and msg->easy_handle == h) { result = msg->data.result; running = 0; }
    if (running) curl_multi_poll(m, nullptr, 0, 1000, nullptr);  // curl_multi_wakeup ends the wait
  }
  curl_multi_remove_handle(m, h);  // always, before the easy handle is reused or cleaned up
  return result;
}

}  // namespace curlx
```

## Using a class member as the callback

libcurl cannot take a member function pointer. Use a static trampoline and pass `this`:

```cpp
class Downloader {
public:
  Downloader() = default;
  Downloader(const Downloader&) = delete;             // `this` is registered with libcurl
  Downloader& operator=(const Downloader&) = delete;

  void start(CURL* h) {
    curlx::setopt<CURLOPT_WRITEFUNCTION>(h, &Downloader::on_write);
    curlx::setopt<CURLOPT_WRITEDATA>(h, this);         // must outlive the transfer
  }
private:
  static std::size_t on_write(char* p, std::size_t, std::size_t n, void* ud) noexcept {
    auto* self = static_cast<Downloader*>(ud);
    try { self->consume({p, n}); return n; }
    catch (...) { self->error_ = std::current_exception(); return CURL_WRITEFUNC_ERROR; }
  }
  void consume(std::string_view chunk);
  std::exception_ptr error_;
};
```

Never move or copy an object whose address is registered as `*DATA` while a transfer might be
running.

## Mapping CURLcode to std::error_code

If the codebase already uses `std::error_code`, add a category instead of passing raw integers:

```cpp
struct CurlCategory final : std::error_category {
  const char* name() const noexcept override { return "libcurl"; }
  std::string message(int ev) const override { return curl_easy_strerror(static_cast<CURLcode>(ev)); }
};
inline const std::error_category& curl_category() { static const CurlCategory c; return c; }
inline std::error_code make_error_code(CURLcode e) { return {static_cast<int>(e), curl_category()}; }
```

Keep the HTTP status separate from the transport code. "Connected, got 429" calls for a retry with
backoff; "could not connect" should fail fast.

## Without C++23

- `std::expected`: return a `struct Result { CURLcode code; long status; std::string message; }`
  instead, or use `tl::expected`.
- `requires { sink.stopped; }`: give every sink a `stopped` member and drop the `if constexpr`.
- Everything else is C++17.
