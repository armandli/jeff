# libcurl API Reference for C++ Code

Facts come from https://curl.se/libcurl/c/. Items marked **(measured)** were checked against the
installed libcurl 8.18.0. "Type" is the C type libcurl reads from the varargs. In C++ that type must
be passed exactly, because curl's compile-time type checker is off for C++.

## Contents

- [Lifecycle](#lifecycle)
- [Common easy options](#common-easy-options)
- [Callbacks](#callbacks)
- [getinfo](#getinfo)
- [Error codes](#error-codes)
- [Multi interface](#multi-interface)
- [Share interface](#share-interface)
- [URL API (CURLU)](#url-api-curlu)
- [Response headers API](#response-headers-api)
- [WebSocket API](#websocket-api)
- [Introspection](#introspection)

## Lifecycle

| Function | Notes |
|---|---|
| `curl_global_init(CURL_GLOBAL_DEFAULT)` | Once, before any other call. Thread-safe since 7.84.0 only if `curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_THREADSAFE` (set in this build). Otherwise, call it before starting any thread. A non-zero return means libcurl is unusable. Never call it from a static initializer or `DllMain`. |
| `curl_global_cleanup()` | Once per init, after the last libcurl use. It does not wait for resolver threads, so don't use libcurl from a module that can be unloaded. |
| `curl_easy_init()` | Returns NULL on failure. The lazy global init it runs is a fail-safe only, not thread-safe. |
| `curl_easy_cleanup(h)` | Frees the handle and closes its connections. Destroy the handle before the objects its `*DATA` options point to (declare those objects first). |
| `curl_easy_reset(h)` | Restores all options to their defaults. Keeps live connections, the TLS session cache, the DNS cache, cookies, shares and alt-svc. |
| `curl_easy_duphandle(h)` | Copies the options, but no connections, cookies or share. Pointer options (POSTFIELDS, HTTPHEADER, …) point at the **same** memory, which must outlive both handles. The source handle must not be in use while it is copied. |
| `curl_easy_perform(h)` | Blocking. Returns a `CURLcode`. Must not be called from inside a callback of the same handle. |

## Common easy options

Defaults are libcurl's. "Copied" means libcurl keeps its own copy of the value.

### Request

| Option | Type | Default / notes |
|---|---|---|
| `CURLOPT_URL` | `char*` | Copied. Max 8 MB. Set `CURLOPT_PROTOCOLS_STR` when the URL is not hard-coded. |
| `CURLOPT_CURLU` | `CURLU*` | Takes precedence over `CURLOPT_URL`. Not copied: keep the `CURLU` alive. |
| `CURLOPT_POSTFIELDS` | `char*` | **Not copied.** Implies POST. Strlen'd unless a size is set. Default Content-Type is `application/x-www-form-urlencoded` **(measured)**. |
| `CURLOPT_POSTFIELDSIZE_LARGE` | `curl_off_t` | The body length. Required for bodies with embedded NULs. `CURLOPT_POSTFIELDSIZE` takes a `long`. |
| `CURLOPT_COPYPOSTFIELDS` | `char*` | Copied. Set the size **before** it, and don't change the size afterwards. |
| `CURLOPT_HTTPHEADER` | `curl_slist*` | **Not copied**: keep it alive while the handle is used. Sent on every request, including redirects. `"Name:"` removes an internal header and `"Name;"` sends an empty one. No CRLF. `Connection:` cannot be overridden. |
| `CURLOPT_CUSTOMREQUEST` | `char*` | Method name (`"PATCH"`, `"DELETE"`). Only changes the method string. The request body still comes from `POSTFIELDS`/`UPLOAD`. |
| `CURLOPT_HTTPGET` | `long` | `1L` switches a reused handle back to GET after a POST. |
| `CURLOPT_NOBODY` | `long` | `1L` sends HEAD. |
| `CURLOPT_UPLOAD` + `CURLOPT_READFUNCTION` | `long`, fn | Streaming PUT. Set `CURLOPT_INFILESIZE_LARGE` (`curl_off_t`) when the size is known. |
| `CURLOPT_MIMEPOST` | `curl_mime*` | Multipart form (see `curl_mime_init`). Free it with `curl_mime_free` after the transfer. |
| `CURLOPT_USERAGENT` | `char*` | Copied. Default: no User-Agent header. |
| `CURLOPT_ACCEPT_ENCODING` | `char*` | `""` advertises every built-in codec (gzip, br, zstd here) and decodes transparently. NULL (the default) sends nothing and does not decode. |
| `CURLOPT_HTTP_VERSION` | `long` | The default uses HTTP/2 over TLS when possible (nghttp2 is built in). |

### Timeouts

| Option | Type | Default / notes |
|---|---|---|
| `CURLOPT_CONNECTTIMEOUT_MS` | `long` | 300 s. Covers DNS, TCP and TLS. Counted inside `TIMEOUT`. |
| `CURLOPT_TIMEOUT_MS` | `long` | 0 (none). The **whole** transfer, including the body. Fails with `CURLE_OPERATION_TIMEDOUT` (28). The last of `TIMEOUT`/`TIMEOUT_MS` set wins. |
| `CURLOPT_LOW_SPEED_LIMIT` / `_TIME` | `long`, `long` | Abort when the average speed stays under LIMIT bytes/s for TIME seconds (28). Use this for streams. The abort lands ≈TIME+6 s after data stops **(measured)**. |
| `CURLOPT_NOSIGNAL` | `long` | Set `1L` in multithreaded programs. |

### Redirects and protocols

| Option | Type | Default / notes |
|---|---|---|
| `CURLOPT_FOLLOWLOCATION` | `long` | 0. `1L` follows. On 301/302/303 a POST becomes a GET without a body. |
| `CURLOPT_MAXREDIRS` | `long` | Limit for `FOLLOWLOCATION`; exceeding it gives `CURLE_TOO_MANY_REDIRECTS` (47). |
| `CURLOPT_PROTOCOLS_STR` | `char*` | All built-in protocols, file:// included **(measured)**. E.g. `"https,http"`. Added in 7.85.0. |
| `CURLOPT_REDIR_PROTOCOLS_STR` | `char*` | HTTP, HTTPS, FTP, FTPS. |
| `CURLOPT_UNRESTRICTED_AUTH` | `long` | 0: `Authorization`/`Cookie` are not sent to a different redirect host. **Other custom headers are sent** **(measured)**. |

### TLS

| Option | Type | Default / notes |
|---|---|---|
| `CURLOPT_SSL_VERIFYPEER` / `VERIFYHOST` | `long` | 1 / 2. Never disable them. |
| `CURLOPT_CAINFO` / `CAPATH` / `CAINFO_BLOB` | `char*` / `char*` / `curl_blob*` | Custom trust store. Blobs are copied unless `CURL_BLOB_NOCOPY` is set. |

### Proxy and connections

| Option | Type | Default / notes |
|---|---|---|
| `CURLOPT_PROXY` | `char*` | Unset: the `http_proxy`/`https_proxy`/`no_proxy` environment variables are honored. `""` disables proxies. |
| `CURLOPT_TCP_KEEPALIVE` | `long` | 0. Turn it on for long idle streams behind NAT. |
| `CURLOPT_FORBID_REUSE` / `FRESH_CONNECT` | `long` | Disable connection reuse. Rarely what you want. |
| `CURLOPT_UNIX_SOCKET_PATH` | `char*` | HTTP over a Unix socket (e.g. a local model server). |

### Output and diagnostics

| Option | Type | Default / notes |
|---|---|---|
| `CURLOPT_WRITEFUNCTION` / `WRITEDATA` | fn / `void*` | Default: `fwrite` to the `FILE*` in `WRITEDATA` (stdout if unset). Always set both in C++. |
| `CURLOPT_HEADERFUNCTION` / `HEADERDATA` | fn / `void*` | One complete header line per call, terminator included. Runs for **every** response, including redirects, 1xx and auth rounds. |
| `CURLOPT_XFERINFOFUNCTION` / `XFERINFODATA` | fn / `void*` | Needs `CURLOPT_NOPROGRESS = 0L`. |
| `CURLOPT_ERRORBUFFER` | `char*` | Must hold at least 256 bytes (`CURL_ERROR_SIZE`); not copied. libcurl clears it at the start of each transfer. |
| `CURLOPT_PRIVATE` | `void*` | Your pointer, read back with `CURLINFO_PRIVATE`. Use it to find a request object from a multi message. |
| `CURLOPT_VERBOSE` / `DEBUGFUNCTION` | `long` / fn | Prints headers, including credentials. Debug builds only. |
| `CURLOPT_MAXFILESIZE_LARGE` | `curl_off_t` | Not sufficient as a size guard on its own; cap the size in the write callback too. |

## Callbacks

All callbacks are C functions. In C++: a `static` member or free function, or `+[](…) noexcept {}`.
They must never throw.

```c
size_t write_cb (char* ptr, size_t size /*always 1*/, size_t nmemb, void* userdata);
size_t header_cb(char* buf, size_t size /*always 1*/, size_t nitems, void* userdata);
size_t read_cb  (char* buf, size_t size, size_t nitems, void* userdata);   // fill up to size*nitems
int    xferinfo (void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
int    debug_cb (CURL* h, curl_infotype type, char* data, size_t size, void* clientp);  // must return 0
```

| Callback | Abort with | Result | Notes |
|---|---|---|---|
| write / header | any count ≠ `nmemb`, or `CURL_WRITEFUNC_ERROR` | `CURLE_WRITE_ERROR` (23) | Data is not NUL-terminated. Write calls carry at most 16 KiB (`CURL_MAX_WRITE_SIZE`), and may carry 0 bytes. A header line can reach 100 KiB. `CURL_WRITEFUNC_PAUSE` pauses (resume with `curl_easy_pause`). Intermediate redirect bodies are not delivered. |
| read | `CURL_READFUNC_ABORT` | `CURLE_ABORTED_BY_CALLBACK` | Return 0 for end of data. `CURL_READFUNC_PAUSE` pauses. |
| xferinfo | non-zero | `CURLE_ABORTED_BY_CALLBACK` (42) | About once a second while idle (1.0 s measured), more often while data flows. `CURL_PROGRESSFUNC_CONTINUE` falls back to the built-in meter. |
| debug | — | — | `curl_infotype`: TEXT 0, HEADER_IN 1, HEADER_OUT 2, DATA_IN 3, DATA_OUT 4, SSL_DATA_IN 5, SSL_DATA_OUT 6. Only runs with `VERBOSE=1L`. Since 8.4.0 the handle may be an internal one. |

## getinfo

Pass a pointer to the exact type:

| Info | Type | Meaning |
|---|---|---|
| `CURLINFO_RESPONSE_CODE` | `long*` | The last HTTP status, or 0 if none was received. Use `CURLINFO_HTTP_CONNECTCODE` for proxy CONNECT. |
| `CURLINFO_CONTENT_TYPE` | `char**` | Points into the handle. Valid until the next transfer or cleanup. Copy it. |
| `CURLINFO_EFFECTIVE_URL` | `char**` | The final URL after redirects. Same lifetime. |
| `CURLINFO_REDIRECT_URL` | `char**` | Where `FOLLOWLOCATION` would have gone (for manual redirect policy). |
| `CURLINFO_TOTAL_TIME_T`, `CONNECT_TIME_T`, `STARTTRANSFER_TIME_T` | `curl_off_t*` | Microseconds. Prefer these over the `double` variants. |
| `CURLINFO_SIZE_DOWNLOAD_T`, `CONTENT_LENGTH_DOWNLOAD_T` | `curl_off_t*` | Bytes. Content length is -1 if unknown. |
| `CURLINFO_NUM_CONNECTS` | `long*` | New connections made by the last transfer. 0 means a reused connection **(measured)**. |
| `CURLINFO_PRIVATE` | `char**` (any pointer type, by convention) | Read back `CURLOPT_PRIVATE`. |
| `CURLINFO_RETRY_AFTER` | `curl_off_t*` | Seconds from a `Retry-After` header (429/503 backoff). |

## Error codes

`curl_easy_strerror(code)` gives a generic message; the error buffer gives the specific one.

| Code | # | Typical cause | Retry? |
|---|---|---|---|
| `CURLE_UNSUPPORTED_PROTOCOL` | 1 | Scheme not built in or disabled by `PROTOCOLS_STR` | no |
| `CURLE_URL_MALFORMAT` | 3 | Bad URL | no |
| `CURLE_COULDNT_RESOLVE_HOST` | 6 | DNS | maybe |
| `CURLE_COULDNT_CONNECT` | 7 | Refused or unreachable | yes, with backoff |
| `CURLE_PARTIAL_FILE` | 18 | Body shorter or longer than its Content-Length | yes |
| `CURLE_HTTP_RETURNED_ERROR` | 22 | Only with `FAILONERROR` | depends on the status |
| `CURLE_WRITE_ERROR` | 23 | Your write callback refused the data (size cap, parse error, deliberate stop) | no |
| `CURLE_OPERATION_TIMEDOUT` | 28 | `TIMEOUT`, `CONNECTTIMEOUT` or `LOW_SPEED` | yes, if idempotent |
| `CURLE_SSL_CONNECT_ERROR` | 35 | TLS handshake | maybe |
| `CURLE_ABORTED_BY_CALLBACK` | 42 | xferinfo or read callback abort (cancellation) | no |
| `CURLE_TOO_MANY_REDIRECTS` | 47 | `MAXREDIRS` | no |
| `CURLE_GOT_NOTHING` | 52 | Server closed the connection without a reply | yes |
| `CURLE_SEND_ERROR` / `CURLE_RECV_ERROR` | 55 / 56 | Connection reset mid-transfer | yes, if idempotent |
| `CURLE_PEER_FAILED_VERIFICATION` | 60 | Certificate rejected | **no**; fix the CA setup, never disable verification |

The multi, share, URL and header APIs have their own code types: `CURLMcode`, `CURLSHcode`,
`CURLUcode` (`curl_url_strerror`, since 7.80.0) and `CURLHcode`.

## Multi interface

Many transfers in one thread, or one transfer you can interrupt.

```cpp
CURLM* m = curl_multi_init();
curl_multi_add_handle(m, easy);                         // repeat per transfer
for (int running = 1; running;) {
    curl_multi_perform(m, &running);
    int queued;
    while (CURLMsg* msg = curl_multi_info_read(m, &queued))
        if (msg->msg == CURLMSG_DONE) {                 // "done" does not mean success
            CURLcode rc = msg->data.result;
            curl_multi_remove_handle(m, msg->easy_handle);  // before reuse or cleanup
        }
    if (running) curl_multi_poll(m, nullptr, 0, 1000, nullptr);  // 7.66.0
}
curl_multi_cleanup(m);                                  // easy handles are freed separately
```

- `curl_multi_wakeup(m)` (7.68.0) can be called from **any thread** and ends a `curl_multi_poll`
  wait. If no poll is in progress, the next one returns immediately. It does not wake
  `curl_multi_wait`.
- The `CURLMsg*` is invalid after `curl_multi_remove_handle`, `curl_easy_cleanup` or
  `curl_multi_cleanup`. Copy `data.result` first.
- Transfers on one multi handle share its connection cache, DNS cache and TLS sessions. Limit them
  with `CURLMOPT_MAX_TOTAL_CONNECTIONS` and `CURLMOPT_MAX_HOST_CONNECTIONS` (`long`).
- `curl_multi_get_handles(m)` returns a NULL-terminated array to be freed with `curl_free`; declared
  in 8.18's `multi.h`.
- For event loops (epoll, Asio, libuv): `CURLMOPT_SOCKETFUNCTION` + `CURLMOPT_TIMERFUNCTION` +
  `curl_multi_socket_action`. Use these only when integrating with an existing loop.

## Share interface

`CURLSH* sh = curl_share_init()`, then `curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_X)`
and `curl_easy_setopt(easy, CURLOPT_SHARE, sh)`.

| Data | Shareable across concurrent threads? |
|---|---|
| `CURL_LOCK_DATA_DNS` | yes, with lock callbacks |
| `CURL_LOCK_DATA_SSL_SESSION` | yes, with lock callbacks |
| `CURL_LOCK_DATA_PSL` (7.61.0) | yes, with lock callbacks |
| `CURL_LOCK_DATA_CONNECT` | **no**: "not supported to share connections between multiple concurrent threads" |
| `CURL_LOCK_DATA_COOKIE` | **no** |
| `CURL_LOCK_DATA_HSTS` (7.88.0) | **no** |

From threads, set `CURLSHOPT_LOCKFUNC`/`CURLSHOPT_UNLOCKFUNC` (one `std::mutex` per
`curl_lock_data` value). Never add share types while the share is in use. Remove it from every easy
handle (`CURLOPT_SHARE`, NULL) before `curl_share_cleanup`. To share connections between threads,
run the transfers on one thread's multi handle instead.

## URL API (CURLU)

```cpp
curlx::Url u(curl_url());
curl_url_set(u.get(), CURLUPART_URL, base, 0);                        // parse, copies input
curl_url_set(u.get(), CURLUPART_QUERY, "q=a b&c", CURLU_APPENDQUERY | CURLU_URLENCODE);
char* out = nullptr;
curl_url_get(u.get(), CURLUPART_URL, &out, 0);                        // free with curl_free
curl_easy_setopt(easy, CURLOPT_CURLU, u.get());                       // or pass the string
```

- `CURLU_URLENCODE` on a query part encodes the value after the first `=`: `q=a+b%26c`
  **(measured)**.
- `CURLU_NON_SUPPORT_SCHEME` allows unknown schemes. Without it, `curl_url_set` rejects them.
- `curl_easy_escape(nullptr, s, len)` / `curl_easy_unescape` return strings that need `curl_free`.
  The handle argument has been ignored since 7.82.0. Prefer `CURLU` for whole URLs.

## Response headers API

`curl_easy_header(h, "content-type", 0, CURLH_HEADER, -1, &hdr)` (7.83.0) reads a header from the
last response without a header callback. The name is case-insensitive and has no colon; request
`-1` means the last request in a redirect chain. The returned `curl_header*` is overwritten by the
next call on the handle and freed by `curl_easy_cleanup`, so copy `hdr->value` into a
`std::string`. Iterate with `curl_easy_nextheader`. Origins: `CURLH_HEADER`, `CURLH_TRAILER`,
`CURLH_CONNECT`, `CURLH_1XX`, `CURLH_PSEUDO`.

## WebSocket API

`ws://`/`wss://` are built in here (8.18 lists both protocols).
- **Callback model:** a normal `perform` delivers frames to `WRITEFUNCTION`. Read frame metadata
  with `curl_ws_meta`.
- **Connect-only model:** `CURLOPT_CONNECT_ONLY = 2L`. Then `curl_easy_perform` performs only the
  upgrade, and you call `curl_ws_send` / `curl_ws_recv` (7.86.0).
  - Both return `CURLE_AGAIN` instead of blocking. Wait for the socket (`CURLINFO_ACTIVESOCKET`)
    and retry with the same arguments.
  - `curl_ws_recv` returns `CURLE_GOT_NOTHING` when the connection is closed. Check
    `meta->bytesleft` for partial frames.
- Pings are answered automatically. Extensions (permessage-deflate) are not supported.

## Introspection

- `curl_version_info(CURLVERSION_NOW)`: version string, feature bits (`CURL_VERSION_THREADSAFE`,
  `CURL_VERSION_ASYNCHDNS`, `CURL_VERSION_HTTP2`, …) and protocols. Log it at startup when
  debugging environment-dependent failures.
- `curl_easy_option_by_name("timeout_ms")` / `curl_easy_option_by_id(CURLOPT_X)` (7.73.0): the
  option's type (`CURLOT_LONG`, `CURLOT_OFF_T`, `CURLOT_STRING`, …). Useful when options come from a
  config file by name.
- `curl-config --version --features --protocols` from the shell.
