# libcurl Pitfalls in C++, and This Repo's Code

## Contents

- [C API gaps and the C++ fix](#c-api-gaps-and-the-c-fix)
- [Behavior that surprises](#behavior-that-surprises)
- [Things libcurl does not do](#things-libcurl-does-not-do)
- [Audit of jfsh's libcurl code](#audit-of-jfshs-libcurl-code)

## C API gaps and the C++ fix

| Gap | Symptom | Fix |
|---|---|---|
| No type checking in C++ (curl.h disables `typecheck-gcc.h` under `__cplusplus`) | `curl_easy_setopt(h, CURLOPT_URL, str)` with a `std::string` compiles with no diagnostic and passes garbage through varargs. `int`/`bool` for a `long` works on x86-64 by accident and breaks elsewhere. `size_t` for `curl_off_t` breaks on 32-bit. | `curlx::setopt<CURLOPT_X>` (static type check), or explicit `1L`, `static_cast<long>`, `static_cast<curl_off_t>` and `.c_str()`. |
| Manual cleanup | A `return`/`throw` between init and cleanup leaks the handle, header list, `CURLU` or `curl_free` string. | `std::unique_ptr` with deleters. |
| `list = curl_slist_append(list, s)` | On OOM, returns NULL: the old list leaks and later headers are silently missing. | Append into a temporary and check it (`curlx::append`). |
| Borrowed pointers | `POSTFIELDS`, `HTTPHEADER`, `ERRORBUFFER`, `*DATA` and `CURLU` dangle once the C++ object dies. A reused handle dangles from the previous request. | Keep the objects alive for the whole transfer, `curl_easy_reset` before every request, and declare the owners before the handle (destruction order). |
| Captureful lambdas | Don't convert to a C function pointer, so they don't compile; casting around this is UB. | A static trampoline plus a context pointer in `*DATA`. A captureless lambda needs unary `+`. |
| Exceptions through C frames | UB: typically `std::terminate`, or skipped libcurl cleanup that leaves the handle corrupt. `std::string::append` and `std::function` calls inside callbacks can throw. | `noexcept` callbacks that catch into `std::exception_ptr`, return `CURL_WRITEFUNC_ERROR` and rethrow after perform. |
| Data not NUL-terminated | `strlen(ptr)` or `printf("%s", ptr)` in a callback reads past the chunk. | `std::string_view(ptr, nmemb)`. |
| Integer error codes | `CURLcode`, `CURLMcode`, `CURLUcode` and `CURLHcode` are different enums that are easy to mix up and to ignore. | `std::expected<T, Error>`, or an `std::error_category` per enum. Mark result-returning wrappers `[[nodiscard]]`. |
| Global init order | Calling `curl_global_init` in a static constructor runs it in unspecified order across translation units (and is forbidden on Windows). Relying on lazy init inside `curl_easy_init` is not thread-safe. | A `GlobalInit` object as the first local in `main`. |
| `std::string_view` for options | Not NUL-terminated: libcurl reads past the view. | Copy into `std::string` and pass `.c_str()`. |

## Behavior that surprises

- **4xx/5xx are success** (`CURLE_OK`). Check `CURLINFO_RESPONSE_CODE`.
- **`CURLOPT_TIMEOUT` includes the body.** Long streams get cut off. Use `LOW_SPEED_LIMIT`/`TIME`
  for streams. Measured on 8.18: a stall aborts ≈TIME+6 s after the last byte.
- **The connect timeout defaults to 300 s.** A dead local server (Ollama, jeff-server) hangs for 5
  minutes unless `CONNECTTIMEOUT_MS` is set.
- **Progress-callback cancellation lags:** the callback runs about once a second when no data
  flows. Use `curl_multi_wakeup` for immediate cancel.
- **POSTFIELDS defaults to `application/x-www-form-urlencoded`.** JSON APIs may reject it or
  mis-parse it. Always send `Content-Type`.
- **Bodies over 1 MiB send `Expect: 100-continue`.** That costs a round trip, or up to a 1 s wait
  when the server ignores it. Add `"Expect:"` to the header list.
- **Options are sticky.** After a POST, a reused handle stays a POST until `curl_easy_reset` or
  `CURLOPT_HTTPGET`.
- **Header callbacks see every response:** 100-continue, redirects and auth rounds. Reset the
  collected headers when a new status line (`HTTP/`) arrives, or use `curl_easy_header(…, -1, …)`
  after the transfer.
- **Redirects forward custom headers.** `Authorization` and `Cookie` are dropped for a new host, but
  `x-api-key` (Anthropic's auth header) is sent to the new host (measured). Keep `FOLLOWLOCATION`
  off for API calls.
- **file:// and many other schemes are enabled** unless `CURLOPT_PROTOCOLS_STR` restricts them. A
  `base_url` from config or the environment could read local files.
- **Proxy environment variables are honored** (`https_proxy`, `no_proxy`). This is usually right,
  but explains "works with curl, fails in tests" differences. `CURLOPT_PROXY ""` disables them.
- **`CURLOPT_NOSIGNAL=1L` hands SIGPIPE to you.** OpenSSL can raise it on a dead connection.
  Programs that keep the default SIGPIPE action die. A shell (jfsh) must ignore SIGPIPE around
  transfers or block it in the transferring thread, while restoring the default for child
  processes.
- **The error buffer is cleared at the start of each transfer** and is filled only for some
  errors. Fall back to `curl_easy_strerror`.

## Things libcurl does not do

| Missing | Do instead |
|---|---|
| JSON | `jeff::json` (simdjson) in this repo. Parse only complete bodies or complete lines. |
| SSE / NDJSON framing | A line splitter over the write callback (`curlx::LineSink`, examples §2–3). |
| Retries and backoff | Your own loop. `CURLINFO_RETRY_AFTER` reads `Retry-After` (examples §7). |
| Rate limiting | Your own token bucket per API key. |
| Async/await or futures | The multi interface on one thread, or a worker thread per blocking call. To integrate with Asio or an epoll loop, use `curl_multi_socket_action` with socket and timer callbacks. |
| Connection sharing between threads | Not supported (`CURL_LOCK_DATA_CONNECT` across concurrent threads). Use one multi handle on one I/O thread and hand requests to it. |
| Zeroing secrets | libcurl does not clear memory before freeing it. Keep API keys in your own buffers and clear them if that matters. |
| Response header map | `curl_easy_header` / `curl_easy_nextheader` (7.83.0), or collect lines in a header callback. |
| A typed C++ API | `curlx.hpp` here, or cpr (a separate dependency; only with the user's agreement). |

## Audit of jfsh's libcurl code

State of `jfsh/src/agent/{anthropic,openai,ollama}.cpp` and `jfsh/src/router/{jeff,jev}.cpp` as of
2026-11-11. Fix these when touching that code; the TODO IDs are from `jfsh/TODO.md`.

| Issue | Where | Fix |
|---|---|---|
| `curl_global_init` is never called (B57) | all | `curlx::GlobalInit` at the top of jfsh's `main`. |
| HTTP status never checked: 401/429/500 JSON error bodies are parsed as responses | every `http_post`, `chat_stream` | Read `CURLINFO_RESPONSE_CODE`. On non-2xx, extract the API's error message and return an `AgentError`/`RouterError` carrying the status. |
| `CURLOPT_TIMEOUT_MS` on streaming requests kills long answers (B46) | `openai.cpp` `chat_stream`, and the future Anthropic/Ollama streaming | `CONNECTTIMEOUT_MS` plus `LOW_SPEED_LIMIT`/`TIME` for streams; keep `TIMEOUT_MS` only for request/response calls. |
| No `CURLOPT_CONNECTTIMEOUT_MS` | all | Set it short for local servers (jeff-server, Ollama): 1–2 s. |
| Manual cleanup: handle and header list leak if code between init and cleanup throws | all | `curlx::Easy` / `curlx::Slist`. |
| Write callbacks are not `noexcept`. `std::string::append` and the user `StreamCallback` (via `on_line`) can throw through libcurl | `curl_write_*`, `curl_stream_write` | Catch into `std::exception_ptr`, return `CURL_WRITEFUNC_ERROR` and rethrow after perform. |
| No response size cap | all `curl_write_*` | Cap in the callback (e.g. 16 MiB) and return `CURL_WRITEFUNC_ERROR`. |
| New handle per request: no connection reuse, a TLS handshake on every line routed | `jeff.cpp`, `jev.cpp`, agents | Keep one `curlx::Easy` per backend or router object and `curl_easy_reset` per request. |
| `curl_slist_append` result not checked | all | `curlx::append`. |
| No scheme restriction on `base_url` / URLs from env or config | all | `CURLOPT_PROTOCOLS_STR "https,http"`. |
| No cancellation of an in-flight agent stream | `chat_stream` | An `std::atomic<bool>` set on SIGINT, with `perform_interruptible` + `curl_multi_wakeup` from a non-signal context, or `set_cancel_flag` for ~1 s latency. |

Things that are already right:
- `std::string(url).c_str()` for `CURLOPT_URL` is safe, because libcurl copies string options.
- `POSTFIELDS` bodies outlive `curl_easy_perform`.
- `openai.cpp` streaming splits lines across chunks with `jeff::json::LineSplitter` and flushes the
  tail with `finish`.
