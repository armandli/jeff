# Crow Limitations and Workarounds (v1.3.5)

Each entry says what Crow does, why it matters, and what to do instead. Entries marked
**(source reading)** come from reading Crow's code and were not reproduced at runtime.

## Contents

- [Request bodies are unbounded and fully buffered](#request-bodies-are-unbounded-and-fully-buffered)
- [Handlers are synchronous](#handlers-are-synchronous)
- [Deferred responses are fragile](#deferred-responses-are-fragile)
- [No graceful shutdown](#no-graceful-shutdown)
- [Bind failures are only logged](#bind-failures-are-only-logged)
- [JSON input errors become 500s](#json-input-errors-become-500s)
- [Query strings stop at 256 parameters](#query-strings-stop-at-256-parameters)
- [No HTTP client, HTTP/2 or HTTP/3](#no-http-client-http2-or-http3)
- [No auth, rate limiting or CSRF protection](#no-auth-rate-limiting-or-csrf-protection)
- [CORS pitfalls](#cors-pitfalls)
- [Process-global logger and template base](#process-global-logger-and-template-base)
- [WebSocket gaps](#websocket-gaps)
- [Documentation errata](#documentation-errata)

## Request bodies are unbounded and fully buffered

The HTTP parser appends every body chunk to `req.body` with no size limit, and middleware and handlers
only run once the whole body has arrived. A client can make the server hold any amount of memory, and
large uploads cannot be streamed to disk. Headers alone are capped at 80 KiB
(`CROW_HTTP_MAX_HEADER_SIZE`).

**Do instead:**
- Put a reverse proxy in front and cap bodies there (`client_max_body_size 10m;` in nginx,
  `LimitRequestBody` in Apache).
- Still check `req.body.size()` in upload handlers and answer 413, so direct access is bounded in what
  gets stored.
- For multi-gigabyte uploads, have the proxy write the file (e.g. nginx `client_body_in_file_only`) or
  use presigned object-storage uploads; don't route the bytes through Crow.

## Handlers are synchronous

There are no coroutine or future-returning handlers. A handler holds a worker thread until it returns,
and `concurrency(N)` gives only N−1 workers (the default, 2, means one). Ten handlers each waiting
200 ms on a database serialize behind each other with one worker.

**Do instead:** size `concurrency()` for the expected number of simultaneous blocking calls; use
connection pools sized to match; move CPU-heavy work to a separate thread pool and wait for it in the
handler with a timeout. Do not call `std::this_thread::sleep_for` or unbounded network calls in
handlers. Crow's 5-second `timeout()` is suspended while a handler runs, so a hung handler holds its
connection forever.

## Deferred responses are fragile

A handler taking `crow::response&` may return without calling `end()` and finish later — Crow's read
loop has an explicit "completed later by user" path, `res.is_alive()` checks whether the client is
still connected, and the official long-polling chat example stores `crow::response*` and ends them
from another request. But:

- It is undocumented, and the official example runs with a single worker thread (`multithreaded()` is
  commented out), where every `end()` happens on the connection's own thread.
- In a v1.3.5 test build with `concurrency(4)`, finishing a deferred response by posting `res.end()`
  from a separate thread back to `*req.io_context` crashed the server (`double free`). The cause was
  not isolated.
- `end()` runs the response write directly on the calling thread; it is not marshalled to the
  connection's io thread.

**Do instead:** compute the response inside the handler. If you need long polling or server push, use
a websocket (whose `send_*` calls are thread-safe) or server-sent events through a proxy-friendly
design. Only use deferred `end()` with a single worker, calling it from Crow's own handler threads, and
always end abandoned responses (check `is_alive()` and a deadline) so connections are not leaked.

## No graceful shutdown

`app.stop()` closes the acceptor and stops every io_context at once: in-flight requests are dropped and
their responses are not sent. Websockets are closed first (non-SSL servers only).

**Do instead:** stop sending traffic first (remove the instance from the load balancer / proxy
upstream), wait for in-flight requests (track a counter in a middleware: increment in `before_handle`,
decrement in `after_handle`), then call `stop()`. For SIGTERM-driven shutdown, `signal_clear()` and
handle the signal yourself to run that sequence.

## Bind failures are only logged

If the port is taken or `bindaddr` is not a valid address, Crow logs an error and `run()` returns
normally — no exception, no exit code. A service can "start" and immediately exit 0.

**Do instead:** treat a return from `run()` as a fatal condition in `main` (`return 1;` after it unless
you called `stop()`), or use `run_async()` and check
`app.wait_for_server_start() == std::cv_status::no_timeout`.

## JSON input errors become 500s

`crow::json::rvalue` throws `std::runtime_error` for a missing key, a wrong type, or a list index out
of range. The default exception handler logs it and sends **500**, so bad client input looks like a
server failure, and monitoring alerts on it.

**Do instead:** validate with `if (!v)`, `v.has(key)` and `v[key].t()` before reading, and return 400;
or throw `crow::bad_request("message")`, which the default handler turns into 400. There is no schema
validation or struct mapping: write a `parse_X(const rvalue&) -> std::optional<X>` per request type.
Never define `CROW_JSON_NO_ERROR_CHECK` for untrusted input — it turns errors into silent garbage.

## Query strings stop at 256 parameters

`crow::query_string` parses at most 256 key/value pairs; the rest are silently ignored. The same parser
backs `req.get_body_params()` for form bodies, so a large HTML form loses fields without an error.

**Do instead:** keep forms small, or send larger forms as JSON or multipart.

## No HTTP client, HTTP/2 or HTTP/3

Crow is a server only and speaks HTTP/1.1 (an `Upgrade: h2c` header is ignored).

**Do instead:** use libcurl or cpr for outbound calls (each blocks a worker — see above); terminate
HTTP/2 and HTTP/3 in the reverse proxy and talk HTTP/1.1 to Crow.

## No auth, rate limiting or CSRF protection

Crow provides building blocks (headers, cookies, sessions, middleware) but no authentication scheme,
rate limiter or CSRF tokens.

**Do instead:**
- Auth: a local or global middleware that checks the `Authorization` header (Basic: base64-decode with
  `crow::utility::base64decode`, split at the first `:`, compare a password **hash**; Bearer: compare in
  constant time) and ends the response with 401. See examples.md §3.
- Rate limiting: prefer the proxy (`limit_req` in nginx). In Crow, a global middleware with a
  mutex-guarded map of token buckets keyed by client address — behind a proxy, key on
  `X-Forwarded-For`, never on `req.remote_ip_address` (which is the proxy).
- CSRF: with cookie sessions, set `SameSite=Lax` or `Strict` and require a custom header or token on
  state-changing requests.

## CORS pitfalls

- `allow_credentials()` with the default origin `*` reflects any request `Origin` (verified in source):
  set `.origin("https://exact.example")` first.
- Rules are matched by **first registered** prefix, not most specific.
- **(source reading)** `cors.blueprint(bp)` stores the blueprint prefix without its leading slash, so it
  does not seem to match any request URL; use `cors.prefix("/" + name)` instead.

## Process-global logger and template base

`crow::logger` (level and handler) and the mustache template base directory are process-wide statics,
shared by every app in the process.

- A blueprint with its own templates directory rewrites the global template base at the start of each
  of its requests; with several worker threads, a concurrent request can load from the wrong directory
  **(source reading)**. Use one base directory with subdirectories instead.
- Running two Crow apps in one process (e.g. public and admin ports) shares log configuration.

## WebSocket gaps

- No permessage-deflate compression.
- Unmasked client frames are accepted unless `CROW_ENFORCE_WS_SPEC` is defined.
- No default message size limit: set `websocket_max_payload()`.
- Websockets are not covered by CORS: check `Origin` in `onaccept`.
- No built-in heartbeat: send `send_ping` periodically (e.g. from `app.tick()`) and drop clients that
  stay silent, or rely on the proxy's read timeout.

## Documentation errata

Mistakes in https://crowcpp.org/master/ as of November 2026, verified against the source:

| Docs say | Actually |
|----------|----------|
| CMake builds default `CROW_ENABLE_SSL`/`CROW_ENABLE_COMPRESSION` to true (Linux setup page) | Both default OFF. |
| Many features are tagged "master" | Most are in v1.3.5; `CROW_JSON_USE_INSERTION_ORDER` is master-only. |
| Testing page: `app1.…run()`, `asio::io_service`, a request line without an HTTP version or `Host` | Use the same `app`, `asio::io_context` (`io_service` is removed by `ASIO_NO_DEPRECATED`), and send `HTTP/1.1` with `Host`. |
| `returnable` sample: private constructor, `this.as_string()` | Make the constructor public and build the string in `dump()`. |
| `get_list` returns `std::vector<std::string>` | Returns `std::vector<char*>` pointing into the request. |
| nginx sample is complete | It lacks `Upgrade`/`Connection` headers, so websockets fail through it. |
| Upload example writes to the part's `filename` | Path traversal: generate stored names. |
| Asio "1.28 or later, up to 1.30" | CI builds against distro `libasio-dev`, Homebrew's current asio, and Boost.Asio. |
