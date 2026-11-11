# Crow Examples (v1.3.5)

These examples target Crow v1.3.5. The behaviors they rely on were checked against its source, and
the status-code and validation behaviors were exercised in a test build. The files below were not each
compiled as written: build and run one before relying on it.

## Contents

- [1. JSON REST service](#1-json-rest-service)
- [2. Safe file upload](#2-safe-file-upload)
- [3. Bearer-token auth as local middleware](#3-bearer-token-auth-as-local-middleware)
- [4. Websocket chat](#4-websocket-chat)
- [5. Organizing routes with blueprints](#5-organizing-routes-with-blueprints)
- [6. In-process route tests (GoogleTest)](#6-in-process-route-tests-googletest)
- [7. Socket integration test](#7-socket-integration-test)
- [8. Deployment: nginx and systemd](#8-deployment-nginx-and-systemd)

## 1. JSON REST service

Routes are registered by a function so tests can build the same app (see example 6).

```cpp
// routes.h
#pragma once
#include "crow.h"
#include <map>
#include <mutex>

struct TodoStore {
    std::mutex mtx;
    std::map<int64_t, std::string> items;
    int64_t next_id = 1;
};

inline crow::json::wvalue todo_json(int64_t id, const std::string& title)
{
    return crow::json::wvalue{{"id", id}, {"title", title}};
}

// Returns the error message, or "" when the body is a valid {"title": non-empty string}.
inline std::string validate_todo(const crow::json::rvalue& body)
{
    if (!body) return "body must be JSON";
    if (body.t() != crow::json::type::Object) return "body must be a JSON object";
    if (!body.has("title") || body["title"].t() != crow::json::type::String)
        return "field 'title' must be a string";
    if (body["title"].s().size() == 0) return "field 'title' must not be empty";
    return "";
}

inline crow::response json_error(int code, const std::string& message)
{
    return crow::response(code, crow::json::wvalue{{"error", message}});
}

template<typename App>
void register_routes(App& app, TodoStore& store)
{
    CROW_ROUTE(app, "/todos")
    ([&store] {
        std::lock_guard<std::mutex> lock(store.mtx);
        crow::json::wvalue list = crow::json::wvalue::list();
        unsigned i = 0;
        for (const auto& [id, title] : store.items) list[i++] = todo_json(id, title);
        return list;
    });

    CROW_ROUTE(app, "/todos/<int>")
    ([&store](int64_t id) {
        std::lock_guard<std::mutex> lock(store.mtx);
        auto it = store.items.find(id);
        if (it == store.items.end()) return json_error(404, "no such todo");
        return crow::response(todo_json(id, it->second));
    });

    CROW_ROUTE(app, "/todos").methods("POST"_method)
    ([&store](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (auto err = validate_todo(body); !err.empty()) return json_error(400, err);
        std::string title(body["title"].s());       // copy out of the rvalue
        std::lock_guard<std::mutex> lock(store.mtx);
        int64_t id = store.next_id++;
        store.items[id] = title;
        crow::response res(201, todo_json(id, title));
        res.set_header("Location", "/todos/" + std::to_string(id));
        return res;
    });

    CROW_ROUTE(app, "/todos/<int>").methods("DELETE"_method)
    ([&store](int64_t id) {
        std::lock_guard<std::mutex> lock(store.mtx);
        return crow::response(store.items.erase(id) ? 204 : 404);
    });
}
```

```cpp
// main.cpp
#include "routes.h"

int main()
{
    crow::SimpleApp app;
    TodoStore store;
    register_routes(app, store);

    app.exception_handler([](crow::response& res) {
        try { throw; }
        catch (const crow::bad_request& e) { res = json_error(400, e.what()); }
        catch (const std::exception& e) {
            CROW_LOG_ERROR << "unhandled: " << e.what();
            res = json_error(500, "internal error");
        }
    });

    app.loglevel(crow::LogLevel::Warning)
       .server_name("")
       .bindaddr("127.0.0.1")
       .port(18080)
       .concurrency(4)
       .run();
}
```

Exercise every path, not just the happy one:

```sh
curl -i localhost:18080/todos -d '{"title":"milk"}'          # 201 + Location
curl -i localhost:18080/todos -d '{"title":5}'               # 400
curl -i localhost:18080/todos -d 'not json'                  # 400
curl -i localhost:18080/todos/999                            # 404
curl -i -X PATCH localhost:18080/todos/1                     # 405
```

## 2. Safe file upload

```cpp
#include "crow.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

int main()
{
    crow::SimpleApp app;
    const fs::path upload_dir = "/var/lib/myapp/uploads";
    constexpr std::size_t kMaxUpload = 10 * 1024 * 1024;   // also enforce in the proxy

    CROW_ROUTE(app, "/upload").methods("POST"_method)
    ([&](const crow::request& req) {
        // The body is already fully in memory here; this only refuses to store it.
        if (req.body.size() > kMaxUpload) return crow::response(413);

        crow::multipart::message_view msg(req);           // throws bad_request (→ 400) if malformed
        auto it = msg.part_map.find("file");
        if (it == msg.part_map.end()) return crow::response(400, "missing form field 'file'");
        const auto& part = it->second;

        // Never use the client's filename as a path: generate the stored name.
        std::string stored = crow::utility::random_alphanum(24);
        std::ofstream out(upload_dir / stored, std::ios::binary);
        if (!out) return crow::response(500);
        out.write(part.body.data(), static_cast<std::streamsize>(part.body.size()));
        if (!out) return crow::response(500);

        // The original name may be kept as metadata — escape it before displaying it anywhere.
        return crow::response(201, crow::json::wvalue{{"id", stored}});
    });

    app.bindaddr("127.0.0.1").port(18080).concurrency(4).run();
}
```

`curl -F file=@photo.jpg localhost:18080/upload` sends a field named `file`.

## 3. Bearer-token auth as local middleware

```cpp
#include "crow.h"

// Compares without an early exit, so timing does not reveal how much of the token matched.
inline bool constant_time_equal(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

struct BearerAuth : crow::ILocalMiddleware
{
    std::string token;              // set once before run(); read-only afterwards

    struct context { bool authenticated = false; };

    void before_handle(crow::request& req, crow::response& res, context& ctx)
    {
        const std::string& header = req.get_header_value("Authorization");
        const std::string prefix = "Bearer ";
        if (header.size() > prefix.size() && header.compare(0, prefix.size(), prefix) == 0
            && constant_time_equal(header.substr(prefix.size()), token)) {
            ctx.authenticated = true;
            return;
        }
        res.code = 401;
        res.set_header("WWW-Authenticate", "Bearer");
        res.end();                  // skips the handler
    }

    void after_handle(crow::request&, crow::response&, context&) {}
};

int main()
{
    crow::App<BearerAuth> app;
    const char* token = std::getenv("API_TOKEN");
    if (!token || !*token) { CROW_LOG_CRITICAL << "API_TOKEN not set"; return 1; }
    app.get_middleware<BearerAuth>().token = token;

    CROW_ROUTE(app, "/health")([] { return "ok"; });              // public

    CROW_ROUTE(app, "/admin/stats")
      .CROW_MIDDLEWARES(app, BearerAuth)                          // protected
      ([] { return crow::json::wvalue{{"uptime_s", 42}}; });

    app.bindaddr("127.0.0.1").port(18080).run();
}
```

## 4. Websocket chat

```cpp
#define CROW_ENFORCE_WS_SPEC
#include "crow.h"
#include <mutex>
#include <unordered_set>

int main()
{
    crow::SimpleApp app;
    std::mutex mtx;
    std::unordered_set<crow::websocket::connection*> clients;

    app.websocket_max_payload(16 * 1024);

    CROW_WEBSOCKET_ROUTE(app, "/chat")
      .onaccept([](const crow::request& req, void**) {
          // Websockets bypass CORS: check the page origin yourself.
          return req.get_header_value("Origin") == "https://chat.example.com";
      })
      .onopen([&](crow::websocket::connection& conn) {
          std::lock_guard<std::mutex> lock(mtx);
          clients.insert(&conn);
      })
      .onclose([&](crow::websocket::connection& conn, const std::string&, uint16_t) {
          std::lock_guard<std::mutex> lock(mtx);
          clients.erase(&conn);                     // conn is destroyed after this returns
      })
      .onmessage([&](crow::websocket::connection& from, const std::string& text, bool is_binary) {
          if (is_binary) return;
          crow::json::wvalue msg{{"from", from.get_remote_ip()}, {"text", text}};
          std::string payload = msg.dump();
          std::lock_guard<std::mutex> lock(mtx);
          for (auto* c : clients) c->send_text(payload);
      });

    app.bindaddr("127.0.0.1").port(18080).multithreaded().run();
}
```

## 5. Organizing routes with blueprints

```cpp
crow::Blueprint api("api");
crow::Blueprint v1("v1");

CROW_BP_ROUTE(v1, "/ping")([] { return "pong"; });         // GET /api/v1/ping
CROW_BP_CATCHALL_ROUTE(v1)([](crow::response& res) {
    res = crow::response(404, crow::json::wvalue{{"error", "unknown v1 endpoint"}});
    res.end();
});

api.register_blueprint(v1);
app.register_blueprint(api);                                // both before app.run()
```

Prefixes are written without slashes. Registering the same prefix twice throws at `run()`.

## 6. In-process route tests (GoogleTest)

`handle_full` runs routing and the handler without a socket. It skips global middleware, cannot test
routes that use **local** middleware (the request has no middleware context), and does not apply
post-processing such as compression or static file sending.

```cpp
#include <gtest/gtest.h>
#include "routes.h"

struct TodoApi : ::testing::Test {
    crow::SimpleApp app;
    TodoStore store;

    void SetUp() override {
        register_routes(app, store);
        app.validate();                                       // required before handle_full
    }

    crow::response call(crow::HTTPMethod method, const std::string& url, std::string body = "") {
        crow::request req;
        req.method = method;
        req.url = url;
        req.raw_url = url;
        req.body = std::move(body);
        crow::response res;
        app.handle_full(req, res);
        return res;
    }
};

TEST_F(TodoApi, CreateThenFetch) {
    auto created = call(crow::HTTPMethod::Post, "/todos", R"({"title":"milk"})");
    ASSERT_EQ(created.code, 201);
    auto id = crow::json::load(created.body)["id"].i();
    auto fetched = call(crow::HTTPMethod::Get, "/todos/" + std::to_string(id));
    EXPECT_EQ(fetched.code, 200);
    EXPECT_EQ(std::string(crow::json::load(fetched.body)["title"].s()), "milk");
}

TEST_F(TodoApi, RejectsBadInputWith400Not500) {
    EXPECT_EQ(call(crow::HTTPMethod::Post, "/todos", "not json").code, 400);
    EXPECT_EQ(call(crow::HTTPMethod::Post, "/todos", R"({"title":5})").code, 400);
    EXPECT_EQ(call(crow::HTTPMethod::Post, "/todos", R"({})").code, 400);
}

TEST_F(TodoApi, UnknownIdIs404) {
    EXPECT_EQ(call(crow::HTTPMethod::Get, "/todos/999").code, 404);
}
```

For query strings, also set `req.url_params = crow::query_string(req.raw_url);` with
`raw_url = "/search?q=x"` and `url = "/search"`.

```cmake
target_link_libraries(todo_tests PRIVATE Crow::Crow GTest::gtest_main)
gtest_discover_tests(todo_tests)
```

## 7. Socket integration test

Tests the real pipeline: parsing, global middleware, headers, compression. Bind to port 0 so parallel
tests never collide, and stop the server on every path.

```cpp
#include <gtest/gtest.h>
#include "crow.h"

struct StopOnExit {                       // app.stop() must run even when an assertion fails
    crow::SimpleApp& app;
    ~StopOnExit() { app.stop(); }
};

// Minimal HTTP/1.1 client. Use boost::asio:: instead of asio:: with CROW_USE_BOOST.
std::string http_get(uint16_t port, const std::string& path)
{
    asio::ip::tcp::iostream s("127.0.0.1", std::to_string(port));
    s << "GET " << path << " HTTP/1.1\r\n"
      << "Host: localhost\r\n"            // required: HTTP/1.1 without Host gets a 400
      << "Connection: close\r\n\r\n" << std::flush;
    return std::string(std::istreambuf_iterator<char>(s), {});
}

TEST(Server, ServesHealth) {
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    CROW_ROUTE(app, "/health")([] { return "ok"; });

    auto running = app.bindaddr("127.0.0.1").port(0).run_async();   // keep the future
    StopOnExit guard{app};
    ASSERT_EQ(app.wait_for_server_start(), std::cv_status::no_timeout) << "bind failed";

    std::string reply = http_get(app.port(), "/health");          // port() is the real port now
    EXPECT_NE(reply.find("HTTP/1.1 200"), std::string::npos);
    EXPECT_EQ(reply.substr(reply.size() - 2), "ok");
}
```

## 8. Deployment: nginx and systemd

nginx in front of Crow (TLS, body limit, websockets):

```nginx
server {
    listen 443 ssl;
    server_name api.example.com;
    ssl_certificate     /etc/letsencrypt/live/api.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/api.example.com/privkey.pem;

    client_max_body_size 10m;            # Crow has no body limit of its own

    location / {
        proxy_pass http://127.0.0.1:18080;
        proxy_http_version 1.1;
        proxy_set_header Host              $host;
        proxy_set_header X-Forwarded-For   $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Upgrade           $http_upgrade;     # websockets
        proxy_set_header Connection        $connection_upgrade;
        proxy_read_timeout 1h;                                # long-lived websockets
    }
}

map $http_upgrade $connection_upgrade {   # http {} context
    default upgrade;
    ''      close;
}
```

Behind the proxy, `req.remote_ip_address` is the proxy's address; read the client from
`X-Forwarded-For` (only because the proxy sets it, and Crow listens on 127.0.0.1 only).

systemd unit:

```ini
[Unit]
Description=My Crow service
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
ExecStart=/opt/myapp/bin/server
WorkingDirectory=/opt/myapp           # static/ and templates/ are resolved from here
User=myapp
Environment=API_TOKEN=change-me
Restart=on-failure
RestartSec=5
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=/var/lib/myapp

[Install]
WantedBy=multi-user.target
```

Crow stops cleanly on SIGTERM (`systemctl stop`) by default.
