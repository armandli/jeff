# Crow Middleware and WebSockets (v1.3.5)

## Contents

- [Writing middleware](#writing-middleware)
- [Order and short-circuiting](#order-and-short-circuiting)
- [Configuring and reaching middleware](#configuring-and-reaching-middleware)
- [Local middleware](#local-middleware)
- [Included: CookieParser](#included-cookieparser)
- [Included: SessionMiddleware](#included-sessionmiddleware)
- [Included: CORSHandler](#included-corshandler)
- [WebSocket routes](#websocket-routes)
- [Connection API and lifetime](#connection-api-and-lifetime)
- [Broadcasting safely](#broadcasting-safely)

## Writing middleware

A middleware is any type with a nested `context` and two hooks:

```cpp
struct RequestTimer
{
    struct context {                       // per-request storage, default-constructed per request
        std::chrono::steady_clock::time_point start;
    };

    void before_handle(crow::request& req, crow::response& res, context& ctx)
    {
        ctx.start = std::chrono::steady_clock::now();
    }

    void after_handle(crow::request& req, crow::response& res, context& ctx)
    {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - ctx.start).count();
        res.set_header("Server-Timing", "app;dur=" + std::to_string(us / 1000.0));
    }
};

crow::App<RequestTimer> app;
```

- **One middleware object serves every request on every thread.** Keep per-request data in `context`;
  any member state (counters, caches, rate-limit tables) needs a mutex or atomics.
- To read another middleware's context, take a fourth template parameter:

  ```cpp
  template<typename AllContext>
  void before_handle(crow::request& req, crow::response& res, context& ctx, AllContext& all)
  {
      auto& cookies = all.template get<crow::CookieParser>();
      auto token = cookies.get_cookie("token");
  }
  ```

  The other middleware must be listed **earlier** in `crow::App<…>`.
- `before_handle` runs after the full request body is read, so middleware cannot reject an oversized
  upload before it is buffered.

## Order and short-circuiting

- Global middleware: `before_handle` in the order listed in `crow::App<A, B, C>`, `after_handle` in
  **reverse** order (C, B, A).
- Local middleware (below) runs after all global `before_handle`s, again in `crow::App<…>` order.
- If a `before_handle` calls `res.end()`, the remaining middlewares and the route handler are skipped;
  `after_handle` still runs for the middlewares already visited. This is how auth guards reject:

  ```cpp
  void before_handle(crow::request& req, crow::response& res, context&)
  {
      if (req.get_header_value("X-Api-Key") != expected_) {
          res.code = 401;
          res.end();
      }
  }
  ```

## Configuring and reaching middleware

```cpp
// Construct with arguments: one argument per middleware, in order.
crow::App<crow::CookieParser, Session> app{Session{crow::FileStore{"/var/lib/app/sessions"}}};

// Or configure after construction, before run():
app.get_middleware<ApiKeyGuard>().set_key(std::getenv("API_KEY"));

// From a route handler: the context of this request.
CROW_ROUTE(app, "/me")([&](const crow::request& req) {
    auto& ctx = app.get_context<RequestTimer>(req);
    …
});
```

`get_context` only works on requests that went through the app's connection pipeline; a hand-built
`crow::request` passed to `handle_full` has no middleware context.

## Local middleware

Middleware that should run only for some routes derives from `crow::ILocalMiddleware`, is listed in
`crow::App<…>` like any other, and is enabled per route or per blueprint:

```cpp
struct AdminOnly : crow::ILocalMiddleware
{
    struct context {};
    void before_handle(crow::request& req, crow::response& res, context&) { … }
    void after_handle(crow::request&, crow::response&, context&) {}
};

crow::App<AdminOnly> app;

CROW_ROUTE(app, "/admin/stats")
  .CROW_MIDDLEWARES(app, AdminOnly)
  ([] { return "stats"; });

crow::Blueprint admin("admin");
admin.CROW_MIDDLEWARES(app, AdminOnly);   // every route in the blueprint
```

A middleware not derived from `ILocalMiddleware` is global; one that is derived runs only where
enabled. Listing a middleware in `CROW_MIDDLEWARES` that is not in the app, or is not local, is a
compile error.

## Included: CookieParser

`#include "crow/middlewares/cookie_parser.h"`

```cpp
crow::App<crow::CookieParser> app;

CROW_ROUTE(app, "/login").methods("POST"_method)([&](const crow::request& req) {
    auto& cookies = app.get_context<crow::CookieParser>(req);
    std::string old = cookies.get_cookie("sid");     // "" if absent
    cookies.set_cookie("sid", new_session_id())
        .path("/")
        .max_age(3600)
        .httponly()
        .secure()                                     // HTTPS only
        .same_site(crow::CookieParser::Cookie::SameSitePolicy::Strict);
    return crow::response(204);
});
```

List `CookieParser` **before** any middleware that reads cookies.

## Included: SessionMiddleware

`#include "crow/middlewares/session.h"`

```cpp
using Session = crow::SessionMiddleware<crow::InMemoryStore>;   // or crow::FileStore{dir}
crow::App<crow::CookieParser, Session> app{Session{
    crow::CookieParser::Cookie("session").path("/").max_age(7 * 24 * 3600)
        .httponly().secure()
        .same_site(crow::CookieParser::Cookie::SameSitePolicy::Lax),
    32,                                   // session id length
    crow::InMemoryStore{}}};

CROW_ROUTE(app, "/visit")([&](const crow::request& req) {
    auto& session = app.get_context<Session>(req);
    session.apply("views", [](int v) { return v + 1; });   // atomic read-modify-write
    session.set("last", req.url);
    return std::to_string(session.get("views", 0));
});
```

- `CookieParser` must come before the session middleware.
- Values may be strings, integers, booleans or doubles: `get(key, fallback)`, `get<bool>(key)`,
  `set`, `remove`, `keys()`, `string(key)`, `refresh_expiration()`, `mutex()`.
- The session object is shared by concurrent requests of the same client; use `apply` or `mutex()`
  for read-modify-write.
- The default cookie (`Session{store}` only) is `session`, path `/`, 30 days, `HttpOnly`,
  `SameSite=Lax`, **no `Secure`** — pass your own cookie, as above, for HTTPS sites. Ids are 20
  alphanumeric characters from `std::random_device` by default.
- `InMemoryStore` loses sessions on restart and is per process; `FileStore` writes one JSON file per
  session and deletes expired files. Neither works across several server processes — write a custom
  store (e.g. Redis) for that.
- Sessions are for small per-client context (logged-in user, flash messages), not durable data.

## Included: CORSHandler

`#include "crow/middlewares/cors.h"`

```cpp
crow::App<crow::CORSHandler> app;
auto& cors = app.get_middleware<crow::CORSHandler>();
cors.global()
      .origin("https://app.example.com")           // set this BEFORE allow_credentials()
      .methods("GET"_method, "POST"_method)
      .headers("Content-Type", "Authorization")
      .max_age(600)
      .allow_credentials()
    .prefix("/internal")                           // most specific prefixes FIRST
      .ignore()                                      // no CORS headers for these routes
    .prefix("/public")
      .origin("*");
```

- Defaults are `*` for origin, methods and headers.
- **With the default origin `*`, `allow_credentials()` echoes back whatever `Origin` the request
  sent** (plus `Vary: Origin`), allowing credentialed requests from any site. Always set an explicit
  origin when credentials are allowed.
- A request uses the **first registered** prefix rule its URL starts with, else `global()`. Register
  `/api/admin` before `/api`, or the broader rule wins.
- Prefer `prefix("/api")` over `blueprint(bp)`: in v1.3.5, `blueprint(bp)` registers the blueprint's
  prefix as stored (`"api"`, no leading slash), while URLs are matched as `"/api/…"`, so from the
  source it appears never to match. Verify with `curl -H "Origin: …"` if you use it.

## WebSocket routes

```cpp
#define CROW_ENFORCE_WS_SPEC   // before any Crow include: reject unmasked client frames
#include "crow.h"

CROW_WEBSOCKET_ROUTE(app, "/ws")
    .max_payload(64 * 1024)                       // bytes; default = app.websocket_max_payload() (unlimited)
    .subprotocols({"chat.v1"})                    // optional; must match a client-requested subprotocol
    .onaccept([&](const crow::request& req, void** userdata) -> bool {
        // Runs on the upgrade request. Return false to refuse the connection.
        auto user = authenticate(req.get_header_value("Authorization"));
        if (!user) return false;
        *userdata = new User(*user);              // retrieved later with conn.userdata()
        return true;
    })
    .onopen([&](crow::websocket::connection& conn) { … })
    .onmessage([&](crow::websocket::connection& conn, const std::string& data, bool is_binary) { … })
    .onerror([&](crow::websocket::connection& conn, const std::string& error) { … })
    .onclose([&](crow::websocket::connection& conn, const std::string& reason, uint16_t code) {
        delete static_cast<User*>(conn.userdata());   // free what onaccept allocated
    });
```

- Events fire in the order accept → open → message* → (error) → close.
- A second `onaccept` form, `void(const crow::request&, std::optional<crow::response>&, void**)`,
  rejects by setting the optional to the response to send (e.g. a 401 with a body).
- Browsers cannot set an `Authorization` header on websocket requests: authenticate with a cookie
  (CookieParser/session) or a short-lived token in the query string, checked in `onaccept`. Check
  `Origin` there too, since websockets are not covered by CORS.
- A message larger than `max_payload` closes the connection and calls `onerror`.
- An exception thrown from `onopen` or `onmessage` is passed to `onerror` and the connection is closed
  with status 1011.
- `mirrorprotocols()` echoes the client's first requested subprotocol instead of matching a list.
- Websocket compression (permessage-deflate) is not supported.
- Global middleware `before_handle` runs on the upgrade request; ending the response there refuses the
  upgrade.
- `app.stop()` closes all open websockets (non-SSL servers).

## Connection API and lifetime

```cpp
conn.send_text("hello");
conn.send_binary(bytes);
conn.send_ping("");  conn.send_pong("");          // pongs to pings are automatic
conn.close("bye", crow::websocket::CloseStatusCode::NormalClosure);
conn.get_remote_ip();  conn.get_remote_port();   // still valid in onclose
conn.get_subprotocol();
conn.userdata(ptr);  conn.userdata();            // one void* per connection
```

- `send_*` and `close` post to the connection's own io thread, so they may be called from any thread.
- The `connection&` you receive is owned by Crow and **destroyed after `onclose` returns**. A stored
  `crow::websocket::connection*` must be removed in `onclose`, under the same mutex every sender holds.
- `onmessage` runs on the connection's io thread; long work there stalls that connection and every
  other connection on the same worker.

## Broadcasting safely

The canonical pattern (from Crow's own `example_ws.cpp`): a mutex-guarded set of connections,
inserted in `onopen`, erased in `onclose`, and iterated under the same lock when sending.

```cpp
std::mutex mtx;
std::unordered_set<crow::websocket::connection*> clients;

CROW_WEBSOCKET_ROUTE(app, "/ws")
    .onopen([&](crow::websocket::connection& conn) {
        std::lock_guard<std::mutex> lock(mtx);
        clients.insert(&conn);
    })
    .onclose([&](crow::websocket::connection& conn, const std::string&, uint16_t) {
        std::lock_guard<std::mutex> lock(mtx);
        clients.erase(&conn);
    })
    .onmessage([&](crow::websocket::connection&, const std::string& data, bool is_binary) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto* c : clients)
            is_binary ? c->send_binary(data) : c->send_text(data);
    });

// Broadcasting from a non-websocket thread (timer, background worker) uses the same lock:
void broadcast(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(mtx);
    for (auto* c : clients) c->send_text(msg);
}
```

Sending only queues the message, so holding the lock while sending is cheap. For many clients or large
messages, copy the string once and pass it to each `send_text`.
