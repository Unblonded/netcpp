# netc++

A cross-platform C++17 network utility library for **Windows, macOS and Linux**.
Everything is used through the arrow operator:

```cpp
#include <netcpp/netcpp.hpp>

auto api = netcpp::api("https://api.example.com");

auto res = api->bearer(token)
              ->timeout(10s)
              ->post("/users", netcpp::Body::json(R"({"name":"bob"})"));

if (res) std::cout << res.status << " " << res.body << "\n";
```

| Area | What you get |
|---|---|
| **HTTP / HTTPS** | `api->get / post / put / patch / del / head / options / request(any verb)`, query params, JSON / form / raw / file / multipart bodies, headers, bearer & basic auth, cookies, redirects, retries with backoff, proxies, client certs, HTTP/2, timeouts |
| **Async** | `api->get_async(...)` → `std::future<Response>`, or callback style `api->send_async(req, on_done, on_error)` |
| **Files** | `api->download(path, file, progress)`, `api->upload(path, file)` — also works for `ftp://` and other libcurl schemes |
| **Streaming** | per-chunk `on_data` callback and `on_progress` (return `false` to cancel) |
| **WebSocket** | `ws://` and `wss://`: `ws->send`, `ws->send_binary`, `ws->receive`, `ws->ping`, `ws->close`, auto-pong, fragmentation, subprotocols |
| **TCP** | `tcp::connect` / `tcp::listen` → `sock->send`, `sock->receive`, `sock->read_line`, `sock->receive_exact`, `server->accept()` |
| **TLS** | `tls::connect(host, 443)` → raw encrypted stream with the same API as TCP |
| **UDP** | `udp::open()` → `udp->send_to`, `udp->receive_from`, broadcast |
| **DNS** | `dns::resolve`, `dns::resolve_one`, `dns::reverse` |
| **Utilities** | port scanner, TCP ping, `wait_for_port`, local interfaces, hostname, public IP, IP validation, URL parse/encode, Base64, SHA-1 |

## Building

Requirements: a C++17 compiler, CMake ≥ 3.16 and **libcurl** (used for HTTP and TLS).

| Platform | Install libcurl |
|---|---|
| macOS | ships with the SDK — nothing to install |
| Ubuntu / Debian | `sudo apt install libcurl4-openssl-dev` |
| Fedora | `sudo dnf install libcurl-devel` |
| Windows | `vcpkg install curl` and pass `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` |

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Options: `NETCPP_BUILD_TESTS`, `NETCPP_BUILD_EXAMPLES`, `NETCPP_INSTALL`, `BUILD_SHARED_LIBS`.

### Using it in your project

```cmake
# as a subdirectory / FetchContent
add_subdirectory(netcpp)
target_link_libraries(my_app PRIVATE netcpp::netcpp)

# or after `cmake --install build`
find_package(netcpp REQUIRED)
target_link_libraries(my_app PRIVATE netcpp::netcpp)
```

## Guide

### HTTP

```cpp
using namespace std::chrono_literals;
auto api = netcpp::api("https://httpbin.org");

// configuration chains with ->
api->header("Accept", "application/json")
   ->user_agent("my-app/2.0")
   ->timeout(15s)
   ->retries(3, 250ms)          // retries network errors, 429, 502, 503, 504
   ->follow_redirects(true);

api->get("/get", {{"q", "search terms"}, {"page", "2"}});   // query params
api->post("/post", netcpp::Body::json(R"({"a":1})"));          // JSON
api->post("/post", {{"user", "bob"}, {"pass", "x"}});         // urlencoded form
api->put("/put", "raw text body");                             // raw string
api->patch("/patch", netcpp::Body::bytes(data, "image/png"));  // binary
api->del("/delete");
api->head("/get");
api->options("/get");
api->request("PROPFIND", "/dav");                              // any verb

// multipart upload
netcpp::Multipart form;
form.field("title", "holiday").file("photo", "beach.jpg", "image/jpeg");
api->post("/upload", form);

// per-request headers / timeout
netcpp::http::Options opts;
opts.headers.set("X-Request-Id", "42");
opts.timeout = 2s;
api->get("/get", {}, opts);

// one-off calls without a client
auto r = netcpp::http::get("https://example.com");
```

`Response` has `status`, `headers`, `body`, `url` (after redirects), `elapsed`,
`ok()`, `header(name)`, `raise_for_status()` and converts to `bool`.

### Async

```cpp
auto f1 = api->get_async("/users/1");
auto f2 = api->get_async("/users/2");
auto u1 = f1.get(), u2 = f2.get();   // run in parallel

netcpp::Request req;
req.method = "POST";
req.url = "/events";
req.body = netcpp::Body::json("{}");
api->send_async(req,
    [](netcpp::Response r) { std::cout << r.status << "\n"; },
    [](std::exception_ptr e) { /* handle */ });
```

### Downloads, uploads, streaming

```cpp
api->download("/big.iso", "big.iso", [](auto got, auto total, auto, auto) {
    std::cout << got << "/" << total << "\r";
    return true;   // false cancels
});
api->upload("/files/report.pdf", "report.pdf");        // PUT
netcpp::api()->upload("ftp://user:pw@host/dir/a.txt", "a.txt");

netcpp::http::Options stream;
stream.on_data = [](const char* p, std::size_t n) { fwrite(p, 1, n, stdout); return true; };
api->get("/stream/100", {}, stream);
```

### WebSocket

```cpp
auto ws = netcpp::ws::connect("wss://ws.postman-echo.com/raw");
ws->send("hello");
ws->send_binary(bytes);
while (auto msg = ws->receive()) {           // nullopt when closed
    std::cout << msg->data << "\n";
    if (msg->data == "bye") ws->close();
}
```

`ws::Options` lets you add headers, subprotocols, a timeout and TLS settings.
`send()` is thread-safe, so one thread can read while others write.

### TCP / TLS / UDP

```cpp
// client
auto sock = netcpp::tcp::connect("example.com", 80);
sock->send("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n");
std::cout << sock->receive_all();

// server
auto server = netcpp::tcp::listen(8080);
while (auto client = server->accept()) {
    while (auto line = client->read_line()) client->send(*line + "\n");
}

// TLS
auto tls = netcpp::tls::connect("example.com", 443);
tls->send("HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
std::cout << *tls->read_line();

// UDP
auto udp = netcpp::udp::open();
udp->send_to("8.8.8.8", 53, query);
if (auto reply = udp->receive_from(2s)) std::cout << reply->address << "\n";
```

Streams also offer `receive(max)`, `receive_exact(n)`, `receive_until(delim)`,
`set_timeout(ms)`, `shutdown_send()`, `remote_address()` and `local_port()`.

### DNS and utilities

```cpp
netcpp::dns::resolve("example.com");                     // all A + AAAA
netcpp::dns::resolve_one("example.com", netcpp::dns::Family::IPv4);
netcpp::dns::reverse("8.8.8.8");                         // "dns.google"

netcpp::util::scan_ports("192.168.1.10", netcpp::util::port_range(1, 1024));
netcpp::util::tcp_ping("example.com", 443);              // optional<double> ms
netcpp::util::is_port_open("localhost", 5432);
netcpp::util::wait_for_port("localhost", 8080, 30s);
netcpp::util::interfaces();                              // local IPs
netcpp::util::hostname();
netcpp::util::public_ip();
```

### Errors

Every failure throws `netcpp::Error`, which carries a `kind()`: `Network`, `Timeout`, `Dns`, `Tls`,
`Http`, `Protocol`, `Closed`, `Aborted`, `InvalidArgument` or `Io`. It also carries a `code()`,
which holds the OS, curl or HTTP code.

```cpp
try {
    api->get("/slow");
} catch (const netcpp::Error& e) {
    if (e.is_timeout()) { /* retry later */ }
}
```

HTTP error statuses (4xx/5xx) do **not** throw by default. Check `res.ok()`, or call `res.raise_for_status()`.

## Examples

`examples/` builds `http_client`, `websocket_chat`, `tcp_echo_server`, `port_scanner` and `netinfo`.

## Tests

`netcpp_tests` runs fully offline against a local HTTP and WebSocket server built with the library itself.
Set `NETCPP_ONLINE=1` to also exercise real HTTPS, TLS and `wss://` endpoints.

## License

MIT
