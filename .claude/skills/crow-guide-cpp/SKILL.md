---
name: crow-guide-cpp
description: Expert reference guide for Crow, the header-only C++ HTTP and WebSocket microframework (CrowCpp, Flask-style CROW_ROUTE). Auto-activates when writing or reviewing C++ server code that uses crow.h, crow::SimpleApp, crow::App, CROW_ROUTE, CROW_WEBSOCKET_ROUTE, CROW_BP_ROUTE, crow::response, crow::json::rvalue or wvalue, crow::mustache, or Crow middleware; or when the user asks how to build a REST API, JSON endpoint, file upload, websocket server, or web page in C++ with Crow, set up Crow with CMake, test Crow routes, or deploy a Crow server. Covers correct handler and return-type rules, the input-validation and threading pitfalls that cause most Crow bugs, middleware, websockets, production hardening, and features Crow lacks (request body limits, HTTP client, HTTP/2, rate limiting, auth) that need custom code. Do NOT use for cpp-httplib (used by this repo's jeff-server), Drogon, Pistache, Boost.Beast, or other C++ web frameworks with no Crow angle, for HTTP client code (libcurl, cpr), or for Python Flask.
---

# Crow C++ Guide

Crow is a header-only, Flask-style C++17 HTTP/WebSocket server framework built on Asio. Routes are
compile-time-checked lambdas; handlers run **synchronously** on a pool of worker threads.

**Pin release `v1.3.5`** (October 2026), never `master`. The docs at https://crowcpp.org/master/ track
master, and their "master" tags are stale: almost everything they tag "master" (TCP_NODELAY, websocket
`max_payload`/`subprotocols`, `get_body_params`, `static_directory`, `crow::bad_request`) is already in
v1.3.5. The one master-only feature found is `CROW_JSON_USE_INSERTION_ORDER`.

## Before You Start: Is Crow the Right Choice?

- **Small/medium REST or websocket service, synchronous handlers** → good fit.
- **Project already uses another framework** (this repo's `jeff-server` uses **cpp-httplib**) → do not
  migrate silently. Mention the tradeoff and let the user decide.
- **Need HTTP/2, an HTTP client, streaming request uploads, or async/coroutine handlers** → Crow has
  none of these. See [references/limitations.md](references/limitations.md).

## CMake Setup

```cmake
find_package(Threads REQUIRED)
include(FetchContent)
FetchContent_Declare(Crow
  GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
  GIT_TAG        v1.3.5
  GIT_SHALLOW    TRUE
)
# set(CROW_ENABLE_SSL ON)          # needs OpenSSL; both default OFF
# set(CROW_ENABLE_COMPRESSION ON)  # needs ZLIB
FetchContent_MakeAvailable(Crow)

add_executable(server main.cpp)
target_compile_features(server PRIVATE cxx_std_17)
target_link_libraries(server PRIVATE Crow::Crow)
```

- Crow needs **Asio headers**: `apt install libasio-dev` / `brew install asio`, or
  `-DASIO_INCLUDE_DIR=…`. Alternatively `set(CROW_USE_BOOST ON)` uses Boost.Asio (Boost ≥ 1.64).
- `Crow::Crow` defines `ASIO_NO_DEPRECATED`: old names like `asio::io_service` (still used in the
  docs' testing page) do not compile — use `asio::io_context`.
- `CROW_ENABLE_SSL` / `CROW_ENABLE_COMPRESSION` default to **OFF** (the Linux setup page's claim that
  they default ON is outdated). Without CMake, define them yourself and link `-lssl -lcrypto` / `-lz`.
- Installed package instead: `find_package(Crow REQUIRED)` + `Crow::Crow`. `crow_all.h` (single header)
  is only for tiny single-file projects.

## Core Rules

Most Crow bugs come from these, not from syntax:

1. **Two handler shapes, two completion rules.** A handler that *returns* a value gets a response built
   and ended for it. A handler that takes `crow::response&` must return `void` and **call `res.end()`
   on every path** — a missed path leaves the client hanging, because Crow stops the connection's
   timeout while it waits for your `end()`.

2. **A returned `int` is a status code, not a body.** `return count;` sends status `count` (an
   undefined code becomes a 500). Return `std::to_string(count)` or a `crow::json::wvalue`.

3. **Untrusted input throws.** `crow::json::rvalue` accessors throw `std::runtime_error` on a missing
   key or wrong type, and the default exception handler turns that into a **500**. Check before reading,
   and reject bad input with a 400:

   ```cpp
   auto body = crow::json::load(req.body);
   if (!body) return crow::response(400, "invalid JSON");
   if (!body.has("n") || body["n"].t() != crow::json::type::Number)
       return crow::response(400, "field 'n' must be a number");
   ```

   Or `throw crow::bad_request("why")` anywhere in a handler: the default handler sends **400** with
   that message as the body.

4. **Handlers run concurrently.** `concurrency(N)` / `multithreaded()` gives N−1 worker threads plus an
   acceptor (minimum 2, so the default is **one worker**). Guard shared state with a mutex. A blocking
   call (DB, sleep, outbound HTTP) occupies that worker; size `concurrency` for your blocking I/O.
   One middleware instance is shared by all threads: put per-request data in its `context`.

5. **Borrowed data dies with its owner.** `req.url_params.get("k")` returns `char*` — **`nullptr`
   when absent**, so `std::string s = req.url_params.get("k");` crashes on a missing key. `r_string`
   from `rvalue::s()` and child `rvalue&`s point into the root `rvalue`. A
   `crow::websocket::connection&` is invalid after its `onclose`.

6. **Configure everything before `run()`.** `run()` validates routes and throws `std::runtime_error`
   for a duplicate route ("handler already exists") or a route without a handler. A bind failure is
   only **logged**: `run()` returns immediately and `run_async()`'s `wait_for_server_start()` times out.

## Canonical Application

```cpp
#include "crow.h"
#include <mutex>
#include <unordered_map>

int main()
{
    crow::SimpleApp app;
    std::mutex mtx;
    std::unordered_map<int64_t, std::string> items;  // shared: guard with mtx
    int64_t next_id = 1;

    CROW_ROUTE(app, "/items/<int>")
    ([&](int64_t id) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = items.find(id);
        if (it == items.end()) return crow::response(404);
        return crow::response(crow::json::wvalue{{"id", id}, {"name", it->second}});
    });

    CROW_ROUTE(app, "/items").methods("POST"_method)
    ([&](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body || !body.has("name") || body["name"].t() != crow::json::type::String)
            return crow::response(400, "expected {\"name\": string}");
        std::lock_guard<std::mutex> lock(mtx);
        int64_t id = next_id++;
        items[id] = std::string(body["name"].s());
        crow::response res(201, crow::json::wvalue{{"id", id}});  // JSON content type
        res.set_header("Location", "/items/" + std::to_string(id));
        return res;
    });

    app.loglevel(crow::LogLevel::Warning)
       .bindaddr("127.0.0.1")   // default 0.0.0.0 exposes every interface
       .port(18080)             // default is 80
       .concurrency(8)
       .run();
}
```

Returning a `crow::json::wvalue` (or `crow::response(wvalue)`) sets `Content-Type: application/json`;
a plain string body does not set a content type unless you do.

## Routes at a Glance

| Feature | Behavior |
|---------|----------|
| URL tags | `<int>` → `int64_t`, `<uint>` → `uint64_t`, `<double>`/`<float>` → `double`, `<string>`/`<str>` → one segment, `<path>` → rest of the URL including `/`. Handler params must match in order; `int` is accepted for `<int>`. A non-matching segment (`/add/x/1` for `<int>`) is a 404. |
| Methods | Default `GET`. `.methods("POST"_method, "PUT"_method)` or `crow::HTTPMethod::Post`. Wrong method → 405. `HEAD` and `OPTIONS` are automatic. |
| Trailing slash | A route declared `/dir/` answers `/dir` with a **301** to `/dir/`. A route declared `/dir` returns 404 for `/dir/`. |
| Not found | 404; customize with `CROW_CATCHALL_ROUTE(app)` (also handles 405). |
| Runtime URLs | `app.route_dynamic(url)` — only when the URL is not a literal; loses compile-time checks. |
| Grouping | `crow::Blueprint bp("prefix")` + `CROW_BP_ROUTE(bp, "/x")`, then `app.register_blueprint(bp)` before `run()`. Prefix has no slashes. |

Every `return` in a handler lambda must have the same type: mixing `return crow::response(404);` with
`return "text";` does not compile, so return `crow::response` everywhere once any path needs a status.

Handler parameter lists, in the order Crow tries them: `(args…)`, `(const crow::request&, args…)`,
`(crow::response&, args…)`, `(const crow::request&, crow::response&, args…)`.

## Error Handling

```cpp
app.exception_handler([](crow::response& res) {
    try { throw; }                                   // rethrow the in-flight exception
    catch (const crow::bad_request& e) { res = crow::response(400, e.what()); }
    catch (const std::exception& e) {
        CROW_LOG_ERROR << "handler failed: " << e.what();
        res = crow::response(500);                   // never echo internal errors to clients
    }
});
```

The handler must assign `res`; Crow ends it. Keep `bad_request` → 400 if you replace the default.

## Production Checklist

- Run behind a reverse proxy (nginx/Apache): `bindaddr("127.0.0.1")`, TLS terminated **in the proxy**
  (do not also enable `CROW_ENABLE_SSL`).
- **Cap request bodies in the proxy** (`client_max_body_size`): Crow buffers the whole body in memory
  with no limit before any middleware or handler runs. Headers are capped at 80 KiB.
- `#define CROW_ENFORCE_WS_SPEC` before including Crow when serving websockets (otherwise unmasked
  client frames are accepted) and set `websocket_max_payload(bytes)` (default: unlimited).
- `CORSHandler`: set an explicit `.origin("https://app.example")` before `.allow_credentials()` — with
  the default origin `*`, credentials mode **reflects any request's `Origin`**.
- Never use a multipart part's `filename` as a filesystem path (the official upload example does —
  path traversal). Generate the stored name yourself.
- `app.server_name("")` to omit the `Server: Crow/…` header; `loglevel(crow::LogLevel::Warning)` to
  stop per-request Info logs.
- Session cookies default to `HttpOnly; SameSite=Lax`, 30 days, **without `Secure`** — add it for HTTPS.
- Crow stops on SIGINT/SIGTERM by default; `signal_clear()` / `signal_add()` change that.

## Capability Boundaries

Crow does not do these; recipes are in [references/limitations.md](references/limitations.md).

| Gap | Instead |
|-----|---------|
| Request body size limit | Enforce in the reverse proxy. |
| Streaming request bodies / large uploads | Body is fully buffered; use the proxy to write uploads to disk, or a different server. |
| HTTP client / outbound requests | libcurl or cpr — and do it off the hot worker, or raise `concurrency`. |
| HTTP/2, HTTP/3 | Terminate in the proxy; Crow speaks HTTP/1.1. |
| Async/coroutine handlers | Handlers are synchronous; deferred `response&` completion is undocumented and fragile (see limitations). |
| Authentication, rate limiting, CSRF | Write middleware (see [references/middleware-and-websockets.md](references/middleware-and-websockets.md)). |
| Request validation / schemas | Check `rvalue` fields by hand, or parse into structs. |
| WebSocket compression | Not supported. |
| Per-app logger or template directory | Both are process-global. |

## Verifying Crow Code

Do not report Crow work as done without exercising it:

1. **Build and run** the server, then `curl` every route — including the failure paths: malformed JSON,
   missing fields, wrong types, unknown IDs, wrong method. Expect 400/404/405, never 500.
2. **Unit-test routes in-process** with `app.validate(); app.handle_full(req, res);` and assert on
   `res.code` and `res.body`. Do not use `handle_full` for routes with **local middleware**: it reads
   `req.middleware_context`, which a hand-built request leaves null.
3. **Integration-test over a socket** with `auto f = app.bindaddr("127.0.0.1").port(0).run_async();`,
   `app.wait_for_server_start()` (check for `std::cv_status::timeout`), then `app.port()` for the real
   port. Call `app.stop()` on **every** path (use a scope guard), or the test hangs.
4. Run under ThreadSanitizer when handlers share state: concurrency bugs are invisible at low load.

Full examples of both test styles: [references/examples.md](references/examples.md).

## References

Load on demand:

- **API reference** — every `App` setting with its default, request/response/query-string API, JSON
  `rvalue`/`wvalue`, mustache, multipart, static files, compression, SSL, logging, base64, compile-time
  macros: [references/api-reference.md](references/api-reference.md)
- **Middleware and websockets** — writing middleware, contexts, local middleware, CookieParser,
  SessionMiddleware, CORSHandler, websocket routes, connection lifetime and broadcasting:
  [references/middleware-and-websockets.md](references/middleware-and-websockets.md)
- **Complete examples** — REST service, safe file upload, auth middleware, websocket chat, blueprints,
  GoogleTest in-process and socket tests, nginx and systemd deployment:
  [references/examples.md](references/examples.md)
- **Limitations and workarounds** — each gap above with what to do, plus documentation errata:
  [references/limitations.md](references/limitations.md)
