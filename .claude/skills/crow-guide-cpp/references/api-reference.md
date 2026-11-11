# Crow API Reference (v1.3.5)

Defaults below were read from the v1.3.5 headers, not the prose docs. `#include "crow.h"` pulls in
everything except the optional middlewares in `crow/middlewares/`.

## Contents

- [App configuration](#app-configuration)
- [Routes](#routes)
- [Request](#request)
- [Query strings](#query-strings)
- [Response](#response)
- [Returning custom types](#returning-custom-types)
- [JSON](#json)
- [Mustache templates](#mustache-templates)
- [Multipart](#multipart)
- [Static files](#static-files)
- [Compression](#compression)
- [SSL/TLS](#ssltls)
- [Logging](#logging)
- [Base64](#base64)
- [Compile-time macros](#compile-time-macros)

## App configuration

`crow::SimpleApp` has no middleware; `crow::App<MW1, MW2, …>` runs middleware in that order. Every
setter returns the app, so calls chain. Set everything **before** `run()`.

| Method | Default | Notes |
|--------|---------|-------|
| `port(uint16_t)` | `80` | `port(0)` picks a free port; `app.port()` returns the real one once started. |
| `bindaddr(std::string)` | `"0.0.0.0"` | All interfaces. Use `"127.0.0.1"` behind a proxy. An invalid address is logged and `run()` returns. |
| `local_socket_path(path)` | off | Serve on a Unix domain socket instead of TCP. `req.remote_port` is 0. |
| `concurrency(n)` | `2` | n−1 worker threads plus the acceptor. Values below 2 become 2. |
| `multithreaded()` | — | `concurrency(std::thread::hardware_concurrency())`. |
| `timeout(uint8_t seconds)` | `5` | Idle/read deadline per connection. Max 255. Not applied while a handler runs. |
| `server_name(std::string)` | `"Crow/<version>"` | Value of the `Server` header; `""` omits it. |
| `loglevel(crow::LogLevel)` | `Info` | Global, not per app (see [Logging](#logging)). |
| `stream_threshold(bytes)` | 1 MiB | Larger response bodies are streamed and exempt from the timeout. |
| `static_directory(path)` | `CROW_STATIC_DIRECTORY` (`"static/"`) | Directory behind the implicit `/static/<path>` route. |
| `tcp_nodelay(bool)` / `websocket_tcp_nodelay(bool)` | off | TCP_NODELAY for HTTP / websocket sockets separately. |
| `websocket_max_payload(bytes)` | unlimited | Global websocket message cap; routes can override. |
| `use_compression(algorithm)` | off | Needs `CROW_ENABLE_COMPRESSION`. |
| `ssl_file(crt, key)` / `ssl_file(pem)` / `ssl_chainfile(fullchain, key)` / `ssl(asio::ssl::context&&)` | off | Need `CROW_ENABLE_SSL`. |
| `signal_clear()` / `signal_add(int)` | SIGINT, SIGTERM | Signals that stop the server. |
| `exception_handler(void(crow::response&))` | 500, or 400 for `crow::bad_request` | Called inside the `catch`; rethrow with `throw;` to inspect. |
| `tick(duration, void())` | off | Runs a function periodically on the server's io thread. |
| `register_blueprint(bp)` | — | See [Routes](#routes). |

Running and stopping:

```cpp
app.run();                          // blocks until stop() or a stop signal
auto fut = app.run_async();         // std::future<void>; KEEP it — a discarded future blocks here
if (app.wait_for_server_start(std::chrono::seconds(3)) == std::cv_status::timeout) { /* bind failed */ }
app.stop();                         // closes the acceptor and websockets; run()/fut return
app.validate();                     // checks routes; run() calls it. Throws std::runtime_error on errors
app.debug_print();                  // logs the routing table at Debug level
```

`run()` throws `std::runtime_error` from `validate()` for: a route without a handler ("no handler for
url"), two routes with the same URL and method ("handler already exists for …"), a route not starting
with `/`, a duplicate blueprint prefix.

## Routes

```cpp
CROW_ROUTE(app, "/users/<uint>/files/<path>")       // URL must be a string literal
  .methods("GET"_method, "DELETE"_method)           // or crow::HTTPMethod::Get, …
  ([](const crow::request& req, uint64_t user, const std::string& file) { … });

CROW_ROUTE(app, "/named")("route_name", [] { return "ok"; });   // optional name, used in errors
```

| Tag | Handler type | Matches |
|-----|--------------|---------|
| `<int>` | `int64_t` (or a narrower signed type) | optional sign + digits |
| `<uint>` | `uint64_t` (or narrower unsigned) | digits |
| `<double>` / `<float>` | `double` | a number |
| `<string>` / `<str>` | `std::string` | one path segment, no `/` |
| `<path>` | `std::string` | the rest of the URL, `/` included |

Methods Crow knows: `DELETE GET HEAD POST PUT CONNECT OPTIONS TRACE PATCH PURGE COPY LOCK MKCOL MOVE
PROPFIND PROPPATCH SEARCH UNLOCK BIND REBIND UNBIND ACL REPORT MKACTIVITY CHECKOUT MERGE NOTIFY
SUBSCRIBE UNSUBSCRIBE MKCALENDAR LINK UNLINK SOURCE`.

- `HEAD` is answered from the `GET` route unless you define `HEAD`. `OPTIONS` is answered
  automatically with an `Allow` header (204, or 200 with the CMake option
  `CROW_RETURNS_OK_ON_HTTP_OPTIONS_REQUEST`); listing `OPTIONS` in `.methods()` has no effect.
- Unknown URL → 404; known URL with the wrong method → 405.
- `CROW_CATCHALL_ROUTE(app)([](const crow::request&, crow::response& res){ … res.end(); })` replaces
  the 404/405 response; `res.code` arrives preset to 404 or 405.
- `app.route_dynamic("/x/<int>")` builds a route from a runtime string; a handler/URL type mismatch
  throws at registration instead of failing to compile.

### Blueprints

```cpp
crow::Blueprint api("api");                  // prefix without slashes → /api/…
crow::Blueprint v1("v1", "v1_static", "v1_templates");  // optional static and templates dirs
CROW_BP_ROUTE(v1, "/users")([] { return "users"; });     // /api/v1/users
CROW_BP_CATCHALL_ROUTE(v1)([] { return "no such v1 endpoint"; });
api.register_blueprint(v1);
app.register_blueprint(api);                 // before run()
api.CROW_MIDDLEWARES(app, AuthMiddleware);   // local middleware for every route in the blueprint
```

A blueprint's static files are served at `/<prefix>/static/<file>` (the endpoint name follows
`CROW_STATIC_ENDPOINT`).

## Request

`crow::request` fields and methods:

| Member | Meaning |
|--------|---------|
| `method` | `crow::HTTPMethod` |
| `url` | Path without the query string. |
| `raw_url` | Path with `?query`. |
| `url_params` | `crow::query_string` (see below). |
| `headers` | `crow::ci_map` — case-insensitive multimap. |
| `body` | Entire body as `std::string` (fully buffered, no size limit). |
| `remote_ip_address`, `remote_port` | Peer address. Behind a proxy this is the **proxy**; read `X-Forwarded-For` yourself and only trust it from the proxy. |
| `http_ver_major`, `http_ver_minor`, `keep_alive`, `close_connection`, `upgrade` | Protocol details. |
| `io_context` | `asio::io_context*` the connection runs on. |
| `get_header_value(name)` | `const std::string&`; empty string when absent. |
| `get_body_params()` | Parses an `application/x-www-form-urlencoded` body into a `query_string`. |

HTTP/1.1 requests without a `Host` header are rejected with 400 before routing.

## Query strings

`req.url_params` is a `const crow::query_string`. Values are **percent-decoded**.

```cpp
const char* name = req.url_params.get("name");            // nullptr if absent — always check
std::string n = name ? name : "";
std::vector<char*> tags = req.url_params.get_list("tag"); // ?tag[]=a&tag[]=b
std::vector<char*> ids  = req.url_params.get_list("id", false);  // ?id=1&id=2
std::unordered_map<std::string, std::string> f = req.url_params.get_dict("f");  // ?f[a]=1&f[b]=2
std::vector<std::string> keys = req.url_params.keys();
```

Only the first **256** key/value pairs are parsed; later ones are silently dropped (this also applies
to `get_body_params()`). `char*` results point into the request; copy them into `std::string` to keep
them. `pop`,
`pop_list` and `pop_dict` remove what they return, so call them on a copy
(`crow::query_string qs = req.url_params;`). If one key is used both as a list and a dict, call
`pop_list` before `get_dict`/`pop_dict`.

## Response

Constructors (all usable as handler return values):

```cpp
crow::response()                                   // 200, empty body
crow::response(404)                                // explicit: status only (default body is the reason phrase)
crow::response("body")                             // 200 with body
crow::response(201, "body")
crow::response("text/html", "<p>hi</p>")           // content type + body ("html", "json", … shorthands or a full MIME type)
crow::response(400, "application/json", R"({"error":"x"})")
crow::response(wvalue)                             // any crow::returnable: JSON, rendered template, multipart
crow::response(201, wvalue)
crow::response(crow::status::NOT_FOUND)            // named status enum
```

A status Crow does not know (e.g. 3 or 123) is replaced by 500. Recognized codes: 100, 101, 200–207,
300–304, 307, 308, 400–410, 412–417, 422–424, 428, 429, 451, 500–504, 506, 507.

Members and methods:

| Member | Use |
|--------|-----|
| `code`, `body` | Status and body; assign directly. |
| `set_header(k, v)` / `add_header(k, v)` / `get_header_value(k)` | Replace / append / read a header. |
| `write(str)` | Append to the body. |
| `end()` / `end(str)` | Finish the response (required in `response&` handlers). Idempotent. |
| `is_completed()` | Whether `end()` was called. |
| `is_alive()` | Whether the client connection is still open (for deferred responses). |
| `redirect(url)` / `redirect_perm(url)` | 307 / 308 with `Location`. |
| `moved(url)` / `moved_perm(url)` | 302 / 301 with `Location`. |
| `set_static_file_info(path, mime = "")` | Send a file (streamed); see [Static files](#static-files). |
| `compressed` | Set `false` to skip compression for this response. |
| `skip_body`, `manual_length_header` | Low-level; Crow sets these for HEAD. |

`crow::response` is move-only.

## Returning custom types

```cpp
struct Csv : crow::returnable {
    std::vector<std::vector<std::string>> rows;
    Csv() : crow::returnable("text/csv") {}
    std::string dump() const override {
        std::string out;
        for (const auto& r : rows) {
            for (size_t i = 0; i < r.size(); ++i) out += (i ? "," : "") + r[i];
            out += '\n';
        }
        return out;
    }
};
CROW_ROUTE(app, "/report.csv")([] { Csv c; c.rows = {{"a", "b"}}; return crow::response(c); });
```

(The docs' sample uses a private constructor and `this.as_string()`, which does not compile.)

## JSON

### Reading: `crow::json::rvalue`

```cpp
crow::json::rvalue v = crow::json::load(req.body);   // copies the input; never throws
if (!v) { /* parse error */ }

v.t();                 // crow::json::type: Null, False, True, Number, String, List, Object
v.has("key");          // object key test (no throw)
v["key"];              // throws if v is not an object or the key is missing
v[0];  v.size();       // list access; throws on non-list / out of range
v.i(); v.u(); v.d();   // int64_t / uint64_t / double; throw on non-number
v.b();                 // bool; throws on non-boolean
v.s();                 // crow::json::detail::r_string (convertible to std::string); throws on non-string
v.nt();                // num_type: Signed_integer, Unsigned_integer, Floating_point
v.keys();              // std::vector<std::string>
for (const auto& item : v) { item.key(); /* for objects */ }
std::string s = std::string(v["name"].s());   // copy before the root rvalue dies
```

- Accessors **throw `std::runtime_error`** on a type mismatch or missing key unless compiled with
  `CROW_JSON_NO_ERROR_CHECK` (then they return garbage/zero — avoid for untrusted input).
- `i()`/`u()` also accept numeric **strings** (`"12"`), which may be looser than you want; check
  `t() == type::Number` first if not.
- Nesting deeper than 1024 is a parse error.
- Duplicate object keys are kept; lookup returns one of them.
- Child values and `r_string`s point into the root `rvalue`; keep the root alive while using them.

### Writing: `crow::json::wvalue`

```cpp
crow::json::wvalue w;
w["name"] = "crow";                          // makes w an object
w["n"] = 42;  w["pi"] = 3.14;  w["ok"] = true;  w["none"] = nullptr;
w["list"][0] = 1;  w["list"][1] = "two";     // operator[](unsigned) grows a list
crow::json::wvalue obj{{"a", 1}, {"b", "x"}};                // object from initializer list
crow::json::wvalue lst = crow::json::wvalue::list({1, 2, 3});
crow::json::wvalue empty = crow::json::wvalue::empty_object();  // {} rather than null
crow::json::wvalue copy(rvalue);             // rvalue → editable wvalue
w.has("name");  w.keys();  w.size();
std::string compact = w.dump();
std::string pretty  = w.dump(2);             // indent with 2 spaces
```

- A default-constructed `wvalue` dumps as `null`; use `empty_object()` for `{}`.
- Assigning by index to a non-list, or by key to a non-object, **discards** its previous content.
- `NaN`/`Inf` serialize as `null`.
- Object key order: `std::unordered_map` (unspecified) by default; `#define CROW_JSON_USE_MAP` sorts
  keys; `CROW_JSON_USE_INSERTION_ORDER` (master only) keeps insertion order. Define before including
  Crow, identically in every translation unit.

## Mustache templates

Templates are loaded relative to the **templates directory**: `templates/` under the process's
current working directory by default.

```cpp
crow::mustache::set_global_base("/srv/app/templates");   // the default base
crow::mustache::set_base("other");                        // changes the CURRENT base — also process-wide
auto page = crow::mustache::load("page.html");            // compiles; path is sanitized
auto raw  = crow::mustache::load_text("plain.html");      // std::string, no mustache processing
auto inl  = crow::mustache::compile("Hello {{name}}");
crow::mustache::context ctx;                              // a crow::json::wvalue
ctx["name"] = "Bob";
ctx["items"] = crow::json::wvalue::list({"a", "b"});      // {{#items}}{{.}}{{/items}}
ctx["shout"] = [](std::string s) { return s + "!"; };     // lambda section
return page.render(ctx);            // crow::returnable with Content-Type text/html
std::string html = page.render_string(ctx);
```

- `{{x}}` HTML-escapes; `{{{x}}}` / `{{& x}}` does not — never use the unescaped forms for user input.
- `load_unsafe` / `load_text_unsafe` skip path sanitizing; avoid with user-influenced names.
- `crow::mustache::set_loader(fn)` replaces file loading (e.g. templates compiled into the binary).
- The template base is a plain process-global string shared by all apps and threads. A blueprint with
  its own templates directory makes Crow rewrite that global at the start of each of its requests,
  which races with other worker threads. With `concurrency > 2`, keep one global base and use
  subdirectories (`load("admin/page.html")`) instead of per-blueprint template directories.
- Load once and reuse the `template_t` in hot paths; `load()` reads and compiles the file every call.

## Multipart

```cpp
crow::multipart::message_view msg(req);      // views into req.body; throws crow::bad_request if malformed
auto it = msg.part_map.find("file");         // part_map: multimap name → part_view
if (it == msg.part_map.end()) return crow::response(400, "missing 'file'");
const auto& part = it->second;
auto cd = part.get_header_object("Content-Disposition");   // .value, .params["name"], .params["filename"]
std::string_view data = part.body;
```

- `crow::multipart::message` owns copies; `message_view` is zero-copy and must not outlive `req`.
- `msg.parts` is the ordered vector; `get_part_by_name(name)` returns one part (an empty part if absent).
- A `message` can be built and returned to send a multipart response.
- The client-supplied `filename` is attacker-controlled: never use it as a path.

## Static files

- **Implicit:** files under `static/` (`CROW_STATIC_DIRECTORY`, or `app.static_directory(dir)`) are
  served at `/static/<path>` (`CROW_STATIC_ENDPOINT`). Paths are sanitized.
- **Explicit:** `CROW_STATIC_FILE(app, "/favicon.ico", "assets/favicon.ico")` or
  `app.static_file("/", "index.html")` — paths are relative to the working directory.
- **From a handler:** `res.set_static_file_info("data/report.pdf", "application/pdf"); res.end();`
  - The file is streamed; any existing `res.body` is ignored.
  - The MIME type comes from the extension (`text/plain` if unknown) unless given; to override, call
    `set_header("Content-Type", …)` **after** `set_static_file_info`.
  - A missing file yields 404.
  - `set_static_file_info_unsafe` skips path sanitizing.
- Static routes are added at `run()`; define `CROW_DISABLE_STATIC_DIR` to turn implicit serving off.
- All paths are relative to the **current working directory** — set it (e.g. systemd
  `WorkingDirectory=`) or use absolute paths.

## Compression

Build with `CROW_ENABLE_COMPRESSION` (CMake option or `-DCROW_ENABLE_COMPRESSION -lz`), then
`app.use_compression(crow::compression::algorithm::GZIP)` (or `DEFLATE`). Responses with a non-empty
body are compressed when the client's `Accept-Encoding` allows; set `res.compressed = false` to opt out
(e.g. already-compressed images). Websocket compression is not supported.

## SSL/TLS

Build with `CROW_ENABLE_SSL` (links OpenSSL), then one of:

```cpp
app.ssl_file("cert.crt", "key.key");
app.ssl_file("combined.pem");
app.ssl_chainfile("fullchain.pem", "privkey.pem");   // Let's Encrypt/certbot: use this, not ssl_file
asio::ssl::context ctx{asio::ssl::context::tls_server};
/* configure ctx */  app.ssl(std::move(ctx));
```

Using `ssl_file` with a full-chain certificate fails with "tlsv1 alert unknown ca". Behind a reverse
proxy, terminate TLS in the proxy and do **not** enable SSL in Crow.

## Logging

```cpp
CROW_LOG_INFO << "started on " << app.port();   // CROW_LOG_{DEBUG,INFO,WARNING,ERROR,CRITICAL}
app.loglevel(crow::LogLevel::Warning);           // Debug < Info < Warning < Error < Critical

class JsonLogger : public crow::ILogHandler {
public:
    void log(const std::string& message, crow::LogLevel level) override {
        // message has no timestamp/level prefix and no trailing newline
        std::cerr << R"({"level":)" << static_cast<int>(level) << R"(,"msg":")" << message << "\"}\n";
    }
};
JsonLogger logger;                      // must outlive the app
crow::logger::setHandler(&logger);
```

- The logger is process-global (shared by every app in the process).
- Timestamps are GMT; `CROW_USE_LOCALTIMEZONE` switches log timestamps (only) to local time.
- `CROW_LOG_LEVEL` (default 1 = Info) sets the initial level at compile time; `CROW_ENABLE_DEBUG`
  starts at Debug.
- At Info level every request and response is logged — use Warning in production.

## Base64

```cpp
std::string enc  = crow::utility::base64encode(s, s.size());
std::string encu = crow::utility::base64encode_urlsafe(s, s.size());
std::string dec  = crow::utility::base64decode(enc, enc.size());   // accepts both alphabets
```

## Compile-time macros

Define before including any Crow header, identically in every translation unit (or via CMake
`target_compile_definitions`).

| Macro | Effect |
|-------|--------|
| `CROW_ENABLE_SSL` | HTTPS support (OpenSSL). |
| `CROW_ENABLE_COMPRESSION` | gzip/deflate support (zlib). |
| `CROW_USE_BOOST` | Boost.Asio instead of standalone Asio. |
| `CROW_ENFORCE_WS_SPEC` | Reject unmasked client websocket frames (RFC 6455). Use in production. |
| `CROW_JSON_USE_MAP` / `CROW_JSON_USE_INSERTION_ORDER` | `wvalue` key order (mutually exclusive; the latter is master-only). |
| `CROW_JSON_NO_ERROR_CHECK` | Remove JSON type checks. Unsafe with untrusted input. |
| `CROW_STATIC_DIRECTORY` / `CROW_STATIC_ENDPOINT` | Implicit static serving (`"static/"`, `"/static/<path>"`). |
| `CROW_DISABLE_STATIC_DIR` | No implicit static route. |
| `CROW_ENABLE_DEBUG` | Initial log level Debug. |
| `CROW_LOG_LEVEL` | Initial log level 0–4 (default 1). |
| `CROW_USE_LOCALTIMEZONE` | Local time in log timestamps. |
| `CROW_HTTP_MAX_HEADER_SIZE` | Header byte limit (default 80 KiB). |
