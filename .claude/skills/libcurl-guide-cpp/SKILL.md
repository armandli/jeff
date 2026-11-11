---
name: libcurl-guide-cpp
description: Expert guide for using libcurl, the C HTTP client library, correctly from C++. Auto-activates when writing or reviewing C++ code that includes curl/curl.h or calls curl_easy_init, curl_easy_setopt, curl_easy_perform, curl_multi_*, curl_slist_append, curl_url or CURLOPT_* options; when the user asks how to make HTTP requests, call a REST or LLM API, stream SSE or NDJSON responses, add timeouts, cancel or parallelize transfers in C++; or when touching jfsh's agent and router HTTP code. Covers RAII ownership for every libcurl handle, type-checked setopt (curl's type checker is disabled in C++), exception-safe callbacks, std::expected error handling, HTTP status checks, streaming timeouts, cancellation, threading, connection reuse, TLS and redirect security, and testing against a local server. Do NOT use for cpp-httplib or Crow server code, for the curl command-line tool, for cpr or other C++ HTTP client libraries with no libcurl angle, or for libcurl from C, Python (pycurl) or PHP.
---

# libcurl in C++

libcurl (https://curl.se/libcurl/c/) is a C library: opaque handles you must free, a variadic
`curl_easy_setopt`, C callbacks with a `void*` user pointer, and integer error codes. Nothing in it
knows about C++ lifetimes or exceptions. Most libcurl bugs in C++ code come from that gap, not from
HTTP. This skill shows how to close it.

Version notes are for **libcurl 8.18.0** (the version installed here; `curl-config --version`).
The online docs track newer releases; check a feature's "Added in" line against the installed version
before using it.

## CMake Setup

```cmake
find_package(CURL REQUIRED)                       # FindCURL ships with CMake
target_link_libraries(app PRIVATE CURL::libcurl)  # imported target carries include dirs
```

Prefer `CURL::libcurl` over `${CURL_INCLUDE_DIRS}`/`${CURL_LIBRARIES}` (what jfsh's
`cmake/Dependencies.cmake` does today). Do not link OpenSSL just for HTTPS: libcurl brings its TLS
backend with it. Add OpenSSL only for code that includes `<openssl/…>` itself.

## Core Rules

1. **Own every handle with RAII.** `CURL*`, `CURLM*`, `CURLU*`, `CURLSH*`, `curl_slist*` and
   strings from `curl_url_get`/`curl_easy_escape` (freed with `curl_free`) all leak on an early
   return or exception. Use `std::unique_ptr` with a deleter struct. jfsh's cleanup is written out by
   hand after `curl_easy_perform`, so any `return` or `throw` placed in between leaks.

   ```cpp
   struct EasyDeleter  { void operator()(CURL* p) const noexcept { curl_easy_cleanup(p); } };
   struct SlistDeleter { void operator()(curl_slist* p) const noexcept { curl_slist_free_all(p); } };
   using Easy  = std::unique_ptr<CURL, EasyDeleter>;
   using Slist = std::unique_ptr<curl_slist, SlistDeleter>;
   ```

   `curl_slist_append` returns NULL on allocation failure **and keeps the old list**, so
   `list = curl_slist_append(list, s)` leaks it. Append into a temporary and check it.

2. **`curl_easy_setopt` is not type-checked in C++.** curl.h disables its type checker for C++
   ("the typechecker does not work in C++"), so all of these compile silently and fail at run time:
   - `std::string` passed for a `char*` option (undefined behavior through varargs);
   - `int`, `bool` or `size_t` passed where the option reads a `long`;
   - `size_t` passed to a `_LARGE` option that reads a `curl_off_t`;
   - a lambda passed where a C function pointer is expected.

   Use the compile-time-checked `setopt<CURLOPT_X>(h, v)` in
   [references/cpp-wrappers.md](references/cpp-wrappers.md). It reads the option's type from its
   number, so wrong types fail to compile. Without the wrapper, pass `1L`/`0L`, `static_cast<long>`,
   `static_cast<curl_off_t>` and `.c_str()` explicitly. Check the returned `CURLcode` for options
   that can fail (`CURLE_UNKNOWN_OPTION`, `CURLE_NOT_BUILT_IN`, `CURLE_BAD_FUNCTION_ARGUMENT`).

3. **Know what libcurl copies.** `char*` string options are **copied**, so
   `std::string(url).c_str()` is safe for `CURLOPT_URL`. Exceptions that must outlive the transfer:

   | Option | Must stay alive until |
   |---|---|
   | `CURLOPT_POSTFIELDS` | the transfer ends (or use `CURLOPT_COPYPOSTFIELDS`) |
   | `CURLOPT_HTTPHEADER` (and other slist options) | the handle is no longer used for transfers |
   | `CURLOPT_ERRORBUFFER` | `curl_easy_cleanup` or the option is reset |
   | `*DATA` user pointers (`WRITEDATA`, `XFERINFODATA`, …) | every transfer that may call the callback |
   | `CURLOPT_CURLU`, `CURLOPT_SHARE` | the handle is done with them |

   Options are **sticky** across transfers on the same handle. A reused handle still points at last
   time's headers and buffers until you reset them (`curl_easy_reset` clears options but keeps
   connections, DNS, TLS sessions and cookies).

4. **Callbacks are C: no captures, no exceptions.** Pass a `static` member function, a free function
   or `+[](…) noexcept {…}` (a captureless lambda converted to a pointer), plus a `this`/context
   pointer in `CURLOPT_*DATA`. An exception thrown through libcurl's C frames is undefined behavior.
   Catch everything inside the callback, store a `std::exception_ptr` in the context, return
   `CURL_WRITEFUNC_ERROR` (the transfer ends with `CURLE_WRITE_ERROR`, 23), and rethrow after
   `curl_easy_perform` returns. Write callbacks get `size == 1` and **non-null-terminated** data of
   any length (including 0). Treat it as `std::string_view(ptr, nmemb)`.

5. **HTTP errors are not libcurl errors.** A 404 or 500 returns `CURLE_OK`. Read
   `CURLINFO_RESPONSE_CODE` (a `long`) after every transfer and treat non-2xx as failure before
   parsing the body. `CURLOPT_FAILONERROR` is not a substitute: it discards the error body (which
   has the API's error message), closes the connection, and the docs say 401/407 can slip through.

6. **Pick timeouts per transfer type.** `CURLOPT_TIMEOUT[_MS]` limits the **whole transfer, body
   included**, so it kills long LLM streams mid-response. jfsh's B46 is this bug.
   - Request/response calls: `CONNECTTIMEOUT_MS` (the default is 300 s) plus a total `TIMEOUT_MS`.
   - Streams: `CONNECTTIMEOUT_MS` plus `LOW_SPEED_LIMIT=1`, `LOW_SPEED_TIME=N` and **no**
     `TIMEOUT`. Measured on 8.18, the stall abort fires about N+6 s after data stops (the speed is a
     moving average), not after N.
   - Use cancellation (below) for user-initiated stops.

7. **Threads: one handle per thread at a time.** Handles may move between threads but never be used
   concurrently. Call `curl_global_init` once in `main` before starting threads; it is thread-safe
   only when `curl_version_info()->features & CURL_VERSION_THREADSAFE` (true here). The lazy init
   inside `curl_easy_init` is not a substitute (jfsh's B57). Set `CURLOPT_NOSIGNAL=1L` in threaded
   programs; this build has the threaded resolver (`AsynchDNS`), so DNS timeouts still work.
   `CURLSH` sharing across threads needs lock callbacks, and **connections and cookies cannot be
   shared between concurrent threads**.

8. **Reuse handles to reuse connections.** Each `curl_easy_init` + `curl_easy_cleanup` pays a new
   TCP+TLS handshake. Keep one handle per thread or per client object, and per remote API where
   possible; a second `perform` on the same handle reused the connection in testing
   (`CURLINFO_NUM_CONNECTS == 0`). Reset per-request options (body, headers, write target) each time.

## Canonical Request

```cpp
#include "curlx.hpp"  // from references/cpp-wrappers.md

std::expected<std::string, curlx::Error>
post_json(CURL* h, const std::string& url, const std::string& body, const std::string& api_key)
{
    curlx::StringSink sink;                     // caps the body at 64 MiB, captures exceptions
    curlx::Slist hdrs;
    curlx::append(hdrs, "Content-Type: application/json");  // default is x-www-form-urlencoded
    curlx::append(hdrs, "Authorization: Bearer " + api_key);
    curlx::append(hdrs, "Expect:");             // no 100-continue round trip for bodies > 1 MiB

    curl_easy_reset(h);                         // reused handle: drop last request's options
    curlx::setopt<CURLOPT_URL>(h, url.c_str());
    curlx::setopt<CURLOPT_HTTPHEADER>(h, hdrs.get());
    curlx::setopt<CURLOPT_POSTFIELDS>(h, body.data());  // not copied: body outlives perform
    curlx::setopt<CURLOPT_POSTFIELDSIZE_LARGE>(h, body.size());
    curlx::setopt<CURLOPT_WRITEFUNCTION>(h, &curlx::StringSink::write);
    curlx::setopt<CURLOPT_WRITEDATA>(h, &sink);
    curlx::setopt<CURLOPT_NOSIGNAL>(h, 1L);
    curlx::setopt<CURLOPT_CONNECTTIMEOUT_MS>(h, 5'000L);
    curlx::setopt<CURLOPT_TIMEOUT_MS>(h, 60'000L);
    curlx::setopt<CURLOPT_PROTOCOLS_STR>(h, "https,http");

    auto r = curlx::perform(h, sink);           // rethrows callback exceptions; non-2xx -> Error
    curlx::setopt<CURLOPT_HTTPHEADER>(h, nullptr);  // hdrs dies at return
    if (not r) {
        r.error().message += ": " + sink.body.substr(0, 512);  // API error text, truncated
        return std::unexpected(std::move(r.error()));
    }
    return std::move(sink.body);
}
```

## Streaming (SSE / NDJSON)

Chunks arrive at arbitrary byte boundaries: one SSE event can span three callbacks, or one callback
can carry five events. Keep the partial line between calls (`curlx::LineSink`), and only parse
complete lines. In this repo, `jeff::json::LineSplitter` (`src/json/json.hpp`) does the same
splitting and is what jfsh's OpenAI backend uses. To stop early (`data: [DONE]`, enough tokens), return false from the line handler;
`curlx::perform(h, sink)` reports that as success instead of `CURLE_WRITE_ERROR`. Full SSE and
Ollama NDJSON clients: [references/examples.md](references/examples.md).

## Cancellation

| Need | Use | Latency |
|---|---|---|
| Stop from another thread, simple | `CURLOPT_XFERINFOFUNCTION` returning 1 when an `std::atomic<bool>` is set (`CURLE_ABORTED_BY_CALLBACK`, 42); needs `NOPROGRESS=0L` | up to ~1 s: the callback runs about once a second while idle (1.00 s measured) |
| Stop immediately (Ctrl-C in a shell, UI cancel) | run the transfer on a `CURLM` and call `curl_multi_wakeup` (callable from any thread) after setting the flag | immediate (0.0 s measured) |

Both patterns are in [references/cpp-wrappers.md](references/cpp-wrappers.md). Never call
`curl_easy_cleanup` on a handle another thread is performing.

## Security Checklist

- Never set `CURLOPT_SSL_VERIFYPEER` or `CURLOPT_SSL_VERIFYHOST` to 0 to silence an error. Fix the
  CA path (`CURLOPT_CAINFO`) instead.
- Restrict schemes for URLs that come from config or users:
  `CURLOPT_PROTOCOLS_STR "https,http"` (8.18 otherwise allows file://, ftp://, smb://, …).
- Redirects are off by default. If you enable `FOLLOWLOCATION`, also set `MAXREDIRS` and
  `REDIR_PROTOCOLS_STR "https"`. libcurl drops `Authorization:` and `Cookie:` on a redirect to
  another host (verified), but **other custom headers such as `x-api-key` are forwarded**. Do not
  follow redirects on requests that carry API keys in custom headers.
- `CURLOPT_VERBOSE` and `CURLOPT_DEBUGFUNCTION` print headers, including API keys. Keep them out of
  release logs, or redact `Authorization`/`x-api-key` in the debug callback.
- Cap response size in the write callback. `CURLOPT_MAXFILESIZE_LARGE` alone is not sufficient (per
  libcurl-security).
- Build URLs with `CURLU` + `CURLU_URLENCODE`, not string concatenation of user text.
- Headers are sent verbatim: reject `\r`/`\n` in any user-supplied header value.

## Verifying libcurl Code

Do not report libcurl work as done without running it against a real server:

1. **Unit tests against an in-process cpp-httplib server** (already a dependency of this repo) bound to
   `127.0.0.1` on an ephemeral port. Test the 2xx path plus at least: a 4xx with a JSON error body,
   connection refused (`http://127.0.0.1:1/`, `CURLE_COULDNT_CONNECT`), a stream split mid-line, and
   a stalled stream (`CURLE_OPERATION_TIMEDOUT`). A GoogleTest fixture is in
   [references/examples.md](references/examples.md).
2. **Build with `-fsanitize=address,undefined`.** Dangling `POSTFIELDS`, freed header lists and
   leaked handles show up immediately.
3. Compile with `-Wall -Wextra -Wconversion`. The checked `setopt` turns option-type mistakes into
   compile errors.
4. To see the wire, set `CURLOPT_VERBOSE=1L` in a debug build only, or run the equivalent `curl -v`
   command line.

## References

Load on demand:

- **C++ wrappers**: the complete, tested `curlx.hpp` (RAII types, checked `setopt`, sinks,
  `perform` returning `std::expected`, URL builder, cancellation, interruptible multi transfer) and
  the design choices behind it: [references/cpp-wrappers.md](references/cpp-wrappers.md)
- **API reference**: option cheat sheet with types, defaults and copy rules; getinfo; error codes;
  multi, share, URL, header and WebSocket APIs with versions:
  [references/api-reference.md](references/api-reference.md)
- **Examples**: JSON GET/POST client class, SSE (OpenAI/Anthropic) and NDJSON (Ollama) streaming,
  parallel requests with the multi interface, a per-thread handle pool, download to a file, GoogleTest
  with a local server: [references/examples.md](references/examples.md)
- **Pitfalls and repo notes**: every gap between the C API and C++ with the fix, and an audit of
  jfsh's current libcurl code: [references/pitfalls.md](references/pitfalls.md)
