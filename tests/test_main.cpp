// netc++ test suite. Runs fully offline against local servers built from the library itself.
// Set NETCPP_ONLINE=1 to also run tests that hit the public internet.

#include <netcpp/netcpp.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// tiny test harness
// ---------------------------------------------------------------------------
namespace {
struct TestCase {
    const char* name;
    std::function<void()> fn;
    bool online;
};
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
struct Registrar {
    Registrar(const char* name, std::function<void()> fn, bool online = false) {
        registry().push_back({name, std::move(fn), online});
    }
};
}  // namespace

#define TEST_IMPL(name, online)                              \
    static void name();                                      \
    static Registrar registrar_##name(#name, name, online);  \
    static void name()
#define TEST(name) TEST_IMPL(name, false)
#define ONLINE_TEST(name) TEST_IMPL(name, true)

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond))                                                                         \
            throw std::runtime_error(std::string("CHECK(" #cond ") failed at line ") +      \
                                     std::to_string(__LINE__));                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto va_ = (a);                                                                  \
        const auto vb_ = (b);                                                                  \
        if (!(va_ == vb_)) {                                                                 \
            std::ostringstream os_;                                                          \
            os_ << "CHECK_EQ(" #a ", " #b ") failed at line " << __LINE__ << ": [" << va_  \
                << "] != [" << vb_ << "]";                                                   \
            throw std::runtime_error(os_.str());                                             \
        }                                                                                    \
    } while (0)

#define CHECK_THROWS_KIND(expr, kind_)                                                       \
    do {                                                                                     \
        bool thrown_ = false;                                                                \
        try {                                                                                \
            (void)(expr);                                                                    \
        } catch (const netcpp::Error& e_) {                                                  \
            thrown_ = true;                                                                  \
            if (e_.kind() != (kind_))                                                        \
                throw std::runtime_error(std::string("wrong error kind at line ") +          \
                                         std::to_string(__LINE__) + ": " + e_.what());       \
        }                                                                                    \
        if (!thrown_)                                                                        \
            throw std::runtime_error("expected exception at line " + std::to_string(__LINE__)); \
    } while (0)

// ---------------------------------------------------------------------------
// Local HTTP + WebSocket test server
// ---------------------------------------------------------------------------
namespace {

std::string read_ws_frame(netcpp::TcpSocket& s, int& opcode) {
    std::string h = s.receive_exact(2);
    opcode = static_cast<unsigned char>(h[0]) & 0x0F;
    bool masked = (static_cast<unsigned char>(h[1]) & 0x80) != 0;
    std::uint64_t len = static_cast<unsigned char>(h[1]) & 0x7F;
    if (len == 126) {
        std::string e = s.receive_exact(2);
        len = (std::uint64_t(std::uint8_t(e[0])) << 8) | std::uint8_t(e[1]);
    } else if (len == 127) {
        std::string e = s.receive_exact(8);
        len = 0;
        for (char c : e) len = (len << 8) | std::uint8_t(c);
    }
    std::string mask = masked ? s.receive_exact(4) : "";
    std::string payload = len ? s.receive_exact(static_cast<std::size_t>(len)) : "";
    if (masked)
        for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
    return payload;
}

void write_ws_frame(netcpp::TcpSocket& s, int opcode, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        f.push_back(126);
        f.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        f.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((std::uint64_t(payload.size()) >> (8 * i)) & 0xFF));
    }
    f += payload;
    s.send(f);
}

class TestServer {
public:
    TestServer() {
        server_ = netcpp::tcp::listen(0, "127.0.0.1");
        port_ = server_->port();
        thread_ = std::thread([this] { loop(); });
    }
    ~TestServer() {
        stop_ = true;
        thread_.join();
    }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::uint16_t port() const { return port_; }

private:
    void loop() {
        while (!stop_) {
            auto client = server_->accept(50ms);
            if (client) std::thread(&TestServer::handle, client).detach();
        }
    }

    static void handle(netcpp::TcpSocket::Ptr c) {
        try {
            c->set_timeout(5s);
            std::string head = c->receive_until("\r\n\r\n");
            std::istringstream in(head);
            std::string method, target, version;
            in >> method >> target >> version;
            netcpp::Headers headers;
            std::string line;
            std::getline(in, line);
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string v = line.substr(colon + 1);
                while (!v.empty() && v.front() == ' ') v.erase(0, 1);
                headers.add(line.substr(0, colon), v);
            }
            std::string body;
            if (headers.has("Content-Length")) {
                body = c->receive_exact(std::stoul(headers.get("Content-Length")));
            }

            std::string path = target.substr(0, target.find('?'));
            int status = 200;
            std::string out_body;
            std::string extra;

            if (path == "/ws") {
                std::string accept = netcpp::base64_encode(
                    netcpp::sha1(headers.get("Sec-WebSocket-Key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
                std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                   "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n";
                if (headers.has("Sec-WebSocket-Protocol")) resp += "Sec-WebSocket-Protocol: chat\r\n";
                c->send(resp + "\r\n");
                c->set_timeout(10s);
                write_ws_frame(*c, 0x9, "srv-ping");  // client must answer with pong
                for (;;) {
                    int op = 0;
                    std::string payload = read_ws_frame(*c, op);
                    if (op == 0x1 || op == 0x2) {
                        write_ws_frame(*c, op, payload);
                    } else if (op == 0x9) {
                        write_ws_frame(*c, 0xA, payload);
                    } else if (op == 0x8) {
                        write_ws_frame(*c, 0x8, payload.substr(0, 2));
                        break;
                    }
                }
                c->close();
                return;
            } else if (path == "/echo") {
                out_body = method + " " + target + "\n" + body;
                extra += "X-Method: " + method + "\r\n";
                extra += "X-Test: " + headers.get("X-Test") + "\r\n";
                extra += "X-Auth: " + headers.get("Authorization") + "\r\n";
                extra += "X-Content-Type: " + headers.get("Content-Type") + "\r\n";
                extra += "X-Cookie: " + headers.get("Cookie") + "\r\n";
                extra += "X-Agent: " + headers.get("User-Agent") + "\r\n";
            } else if (path.rfind("/status/", 0) == 0) {
                status = std::stoi(path.substr(8));
                out_body = "status";
            } else if (path == "/redirect") {
                status = 302;
                extra += "Location: /echo\r\n";
            } else if (path == "/slow") {
                std::this_thread::sleep_for(1500ms);
                out_body = "slow";
            } else if (path == "/big") {
                out_body.assign(1 << 20, 'x');
            } else if (path == "/setcookie") {
                extra += "Set-Cookie: session=abc123; Path=/\r\n";
                out_body = "ok";
            } else {
                status = 404;
                out_body = "not found";
            }

            std::string resp = "HTTP/1.1 " + std::to_string(status) + " X\r\n";
            resp += "Content-Length: " + std::to_string(out_body.size()) + "\r\n";
            resp += "Connection: close\r\n" + extra + "\r\n";
            if (method != "HEAD") resp += out_body;
            c->send(resp);
            c->shutdown_send();
            c->close();
        } catch (...) {
        }
    }

    netcpp::TcpServer::Ptr server_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

TestServer& server() {
    static TestServer s;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// encoding / url
// ---------------------------------------------------------------------------
TEST(base64_roundtrip) {
    CHECK_EQ(netcpp::base64_encode("hello"), std::string("aGVsbG8="));
    CHECK_EQ(netcpp::base64_encode(""), std::string(""));
    CHECK_EQ(netcpp::base64_encode("ab"), std::string("YWI="));
    CHECK_EQ(netcpp::base64_decode("aGVsbG8="), std::string("hello"));
    std::string bin("\x00\xff\x10\x80", 4);
    CHECK_EQ(netcpp::base64_decode(netcpp::base64_encode(bin)), bin);
}

TEST(sha1_vectors) {
    CHECK_EQ(netcpp::hex_encode(netcpp::sha1("abc")),
             std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    CHECK_EQ(netcpp::hex_encode(netcpp::sha1("")),
             std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
}

TEST(url_encoding) {
    CHECK_EQ(netcpp::url_encode("a b&c=d/é"), std::string("a%20b%26c%3Dd%2F%C3%A9"));
    CHECK_EQ(netcpp::url_decode("a%20b+c"), std::string("a b c"));
    CHECK_EQ(netcpp::build_query({{"q", "net c++"}, {"n", "1"}}), std::string("q=net%20c%2B%2B&n=1"));
    auto params = netcpp::parse_query("?a=1&b=two%20words&c");
    CHECK_EQ(params.size(), 3u);
    CHECK_EQ(params[1].second, std::string("two words"));
}

TEST(url_parse) {
    auto u = netcpp::Url::parse("https://user:pw@example.com:8443/a/b?x=1#frag");
    CHECK_EQ(u.scheme, std::string("https"));
    CHECK_EQ(u.user, std::string("user"));
    CHECK_EQ(u.password, std::string("pw"));
    CHECK_EQ(u.host, std::string("example.com"));
    CHECK_EQ(u.port, 8443);
    CHECK_EQ(u.path, std::string("/a/b"));
    CHECK_EQ(u.query, std::string("x=1"));
    CHECK_EQ(u.fragment, std::string("frag"));
    CHECK_EQ(u.target(), std::string("/a/b?x=1"));
    auto v6 = netcpp::Url::parse("ws://[::1]:9000");
    CHECK_EQ(v6.host, std::string("::1"));
    CHECK_EQ(v6.host_port(), std::string("[::1]:9000"));
    CHECK_EQ(netcpp::Url::parse("wss://x.io").effective_port(), 443);
    CHECK_EQ(netcpp::join_url("http://a/", "/b"), std::string("http://a/b"));
    CHECK_EQ(netcpp::join_url("http://a", "b"), std::string("http://a/b"));
    CHECK_THROWS_KIND(netcpp::Url::parse("nonsense"), netcpp::ErrorKind::InvalidArgument);
}

// ---------------------------------------------------------------------------
// sockets
// ---------------------------------------------------------------------------
TEST(tcp_echo_roundtrip) {
    auto srv = netcpp::tcp::listen(0, "127.0.0.1");
    std::thread t([srv] {
        auto c = srv->accept(5s);
        while (auto line = c->read_line()) c->send("echo:" + *line + "\n");
    });
    auto sock = netcpp::tcp::connect("127.0.0.1", srv->port());
    sock->set_no_delay();
    sock->send("hello\r\nworld\n");
    CHECK_EQ(*sock->read_line(), std::string("echo:hello"));
    CHECK_EQ(*sock->read_line(), std::string("echo:world"));
    CHECK_EQ(sock->remote_port(), srv->port());
    sock->shutdown_send();
    CHECK(!sock->read_line().has_value());
    t.join();
}

TEST(tcp_receive_timeout) {
    auto srv = netcpp::tcp::listen(0, "127.0.0.1");
    auto sock = netcpp::tcp::connect("127.0.0.1", srv->port());
    auto peer = srv->accept(2s);
    CHECK(peer != nullptr);
    sock->set_timeout(150ms);
    CHECK_THROWS_KIND(sock->receive(), netcpp::ErrorKind::Timeout);
}

TEST(tcp_connect_refused) {
    std::uint16_t port;
    {
        auto srv = netcpp::tcp::listen(0, "127.0.0.1");
        port = srv->port();
    }
    CHECK_THROWS_KIND(netcpp::tcp::connect("127.0.0.1", port, 2s), netcpp::ErrorKind::Network);
}

TEST(accept_timeout_returns_null) {
    auto srv = netcpp::tcp::listen(0, "127.0.0.1");
    CHECK(srv->accept(50ms) == nullptr);
}

TEST(udp_roundtrip) {
    auto a = netcpp::udp::open(0, "127.0.0.1");
    auto b = netcpp::udp::open(0, "127.0.0.1");
    a->send_to("127.0.0.1", b->local_port(), "ping");
    auto d = b->receive_from(2s);
    CHECK(d.has_value());
    CHECK_EQ(d->data, std::string("ping"));
    CHECK_EQ(d->port, a->local_port());
    b->send_to(d->address, d->port, "pong");
    auto r = a->receive_from(2s);
    CHECK(r && r->data == "pong");
    CHECK(!a->receive_from(50ms).has_value());
}

TEST(dns_localhost) {
    auto addrs = netcpp::dns::resolve("localhost");
    CHECK(!addrs.empty());
    CHECK_EQ(netcpp::dns::resolve_one("127.0.0.1", netcpp::dns::Family::IPv4), std::string("127.0.0.1"));
    CHECK_THROWS_KIND(netcpp::dns::resolve("no-such-host.invalid"), netcpp::ErrorKind::Dns);
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
TEST(http_get_with_params_and_headers) {
    auto api = netcpp::api(server().base());
    auto res = api->header("X-Test", "yes")->user_agent("tester/1")->get("/echo", {{"q", "a b"}, {"n", "1"}});
    CHECK_EQ(res.status, 200L);
    CHECK(res.ok());
    CHECK_EQ(res.header("X-Method"), std::string("GET"));
    CHECK_EQ(res.header("x-test"), std::string("yes"));
    CHECK_EQ(res.header("X-Agent"), std::string("tester/1"));
    CHECK(res.body.find("GET /echo?q=a%20b&n=1") == 0);
}

TEST(http_post_json_put_patch_delete) {
    auto api = netcpp::api(server().base());
    auto r1 = api->post("/echo", netcpp::Body::json(R"({"a":1})"));
    CHECK_EQ(r1.header("X-Method"), std::string("POST"));
    CHECK_EQ(r1.header("X-Content-Type"), std::string("application/json"));
    CHECK(r1.body.find(R"({"a":1})") != std::string::npos);

    auto r2 = api->put("/echo", "raw-body");
    CHECK_EQ(r2.header("X-Method"), std::string("PUT"));
    CHECK(r2.body.find("raw-body") != std::string::npos);

    auto r3 = api->patch("/echo", {{"name", "bob"}, {"age", "3"}});
    CHECK_EQ(r3.header("X-Method"), std::string("PATCH"));
    CHECK_EQ(r3.header("X-Content-Type"), std::string("application/x-www-form-urlencoded"));
    CHECK(r3.body.find("name=bob&age=3") != std::string::npos);

    auto r4 = api->del("/echo");
    CHECK_EQ(r4.header("X-Method"), std::string("DELETE"));

    auto r5 = api->head("/echo");
    CHECK_EQ(r5.status, 200L);
    CHECK(r5.body.empty());

    auto r6 = api->options("/echo");
    CHECK_EQ(r6.header("X-Method"), std::string("OPTIONS"));

    auto r7 = api->request("PROPFIND", "/echo");
    CHECK_EQ(r7.header("X-Method"), std::string("PROPFIND"));
}

TEST(http_auth_and_status) {
    auto api = netcpp::api(server().base());
    CHECK_EQ(api->bearer("tok")->get("/echo").header("X-Auth"), std::string("Bearer tok"));
    auto basic = netcpp::api(server().base())->basic_auth("u", "p")->get("/echo");
    CHECK_EQ(basic.header("X-Auth"), std::string("Basic ") + netcpp::base64_encode("u:p"));

    auto nf = api->get("/status/404");
    CHECK_EQ(nf.status, 404L);
    CHECK(!nf.ok());
    CHECK_THROWS_KIND(nf.raise_for_status(), netcpp::ErrorKind::Http);
}

TEST(http_redirect_and_cookies) {
    auto api = netcpp::api(server().base());
    auto r = api->get("/redirect");
    CHECK_EQ(r.status, 200L);
    CHECK(r.url.find("/echo") != std::string::npos);

    api->get("/setcookie");
    CHECK(api->get("/echo").header("X-Cookie").find("session=abc123") != std::string::npos);

    auto no_follow = netcpp::api(server().base())->follow_redirects(false)->get("/redirect");
    CHECK_EQ(no_follow.status, 302L);
}

TEST(http_timeout) {
    auto api = netcpp::api(server().base())->timeout(300ms);
    CHECK_THROWS_KIND(api->get("/slow"), netcpp::ErrorKind::Timeout);
}

TEST(http_async_and_parallel) {
    auto api = netcpp::api(server().base());
    std::vector<std::future<netcpp::Response>> futures;
    for (int i = 0; i < 8; ++i) futures.push_back(api->get_async("/echo", {{"i", std::to_string(i)}}));
    for (int i = 0; i < 8; ++i) {
        auto r = futures[i].get();
        CHECK(r.body.find("i=" + std::to_string(i)) != std::string::npos);
    }
    std::promise<long> done;
    netcpp::Request req;
    req.url = "/echo";
    api->send_async(req, [&](netcpp::Response r) { done.set_value(r.status); },
                    [&](std::exception_ptr e) { done.set_exception(e); });
    CHECK_EQ(done.get_future().get(), 200L);
}

TEST(http_streaming_progress_download_upload) {
    auto api = netcpp::api(server().base());
    std::size_t streamed = 0;
    netcpp::http::Options opts;
    opts.on_data = [&](const char*, std::size_t n) { streamed += n; return true; };
    auto r = api->get("/big", {}, opts);
    CHECK_EQ(streamed, std::size_t(1 << 20));
    CHECK(r.body.empty());

    auto tmp = std::filesystem::temp_directory_path() / "netcpp_test_download.bin";
    bool progressed = false;
    auto d = api->download("/big", tmp.string(), [&](std::uint64_t now, std::uint64_t, std::uint64_t, std::uint64_t) {
        if (now > 0) progressed = true;
        return true;
    });
    CHECK(d.ok());
    CHECK(progressed);
    CHECK_EQ(std::filesystem::file_size(tmp), std::uintmax_t(1 << 20));

    auto missing = api->download("/status/404", tmp.string());
    CHECK_EQ(missing.status, 404L);
    CHECK(!std::filesystem::exists(tmp));

    {
        std::ofstream f(tmp, std::ios::binary);
        f << "file-contents-123";
    }
    auto up = api->upload("/echo", tmp.string());
    CHECK_EQ(up.header("X-Method"), std::string("PUT"));
    CHECK(up.body.find("file-contents-123") != std::string::npos);

    netcpp::Multipart form;
    form.field("title", "hello").file("upload", tmp.string(), "text/plain");
    auto mp = api->post("/echo", form);
    CHECK(mp.header("X-Content-Type").find("multipart/form-data") == 0);
    CHECK(mp.body.find("hello") != std::string::npos);
    CHECK(mp.body.find("file-contents-123") != std::string::npos);
    std::filesystem::remove(tmp);

    netcpp::http::Options abort_opts;
    abort_opts.on_data = [](const char*, std::size_t) { return false; };
    CHECK_THROWS_KIND(api->get("/big", {}, abort_opts), netcpp::ErrorKind::Aborted);
}

TEST(http_errors) {
    CHECK_THROWS_KIND(netcpp::api("http://no-such-host.invalid")->get("/"), netcpp::ErrorKind::Dns);
    std::uint16_t port;
    {
        auto srv = netcpp::tcp::listen(0, "127.0.0.1");
        port = srv->port();
    }
    CHECK_THROWS_KIND(netcpp::api("http://127.0.0.1:" + std::to_string(port))->get("/"),
                      netcpp::ErrorKind::Network);
}

TEST(http_free_functions) {
    auto r = netcpp::http::get(server().base() + "/echo");
    CHECK(r.ok());
    CHECK_EQ(netcpp::http::post(server().base() + "/echo", "x").header("X-Method"), std::string("POST"));
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------
TEST(websocket_echo) {
    netcpp::ws::Options opts;
    opts.protocols = {"chat"};
    auto ws = netcpp::ws::connect("ws://127.0.0.1:" + std::to_string(server().port()) + "/ws", opts);
    CHECK(ws->is_open());
    CHECK_EQ(ws->protocol(), std::string("chat"));
    ws->set_timeout(5s);

    ws->send("hello ws");
    auto m = ws->receive();  // the server's ping is answered transparently first
    CHECK(m && m->is_text());
    CHECK_EQ(m->data, std::string("hello ws"));

    std::string big(70000, 'z');
    ws->send_binary(big);
    auto b = ws->receive();
    CHECK(b && !b->is_text());
    CHECK_EQ(b->data.size(), big.size());

    ws->ping("p");
    ws->send("after-ping");
    CHECK_EQ(ws->receive()->data, std::string("after-ping"));

    ws->close(1000, "bye");
    CHECK(!ws->receive().has_value());
    CHECK(!ws->is_open());
    CHECK_EQ(ws->close_code(), 1000);
}

// ---------------------------------------------------------------------------
// utilities
// ---------------------------------------------------------------------------
TEST(utils_basic) {
    CHECK(!netcpp::util::hostname().empty());
    auto ifs = netcpp::util::interfaces();
    CHECK(!ifs.empty());
    CHECK(netcpp::util::is_ipv4("192.168.1.1"));
    CHECK(!netcpp::util::is_ipv4("300.1.1.1"));
    CHECK(netcpp::util::is_ipv6("::1"));
    CHECK(!netcpp::util::is_ipv6("hello"));
    CHECK(netcpp::util::version().find("netc++") == 0);
    CHECK_EQ(netcpp::util::port_range(10, 12).size(), 3u);
}

TEST(utils_ports) {
    auto srv = netcpp::tcp::listen(0, "127.0.0.1");
    std::uint16_t p = srv->port();
    CHECK(netcpp::util::is_port_open("127.0.0.1", p));
    auto open = netcpp::util::scan_ports("127.0.0.1", {p, static_cast<std::uint16_t>(p == 65535 ? 1 : p + 1)}, 300ms);
    CHECK(std::find(open.begin(), open.end(), p) != open.end());
    auto rtt = netcpp::util::tcp_ping("127.0.0.1", p);
    CHECK(rtt.has_value() && *rtt >= 0.0);
    CHECK(netcpp::util::wait_for_port("127.0.0.1", p, 1s));
}

// ---------------------------------------------------------------------------
// online (opt-in)
// ---------------------------------------------------------------------------
ONLINE_TEST(online_https_get) {
    auto api = netcpp::api("https://httpbin.org")->timeout(15s);
    auto r = api->get("/get", {{"lib", "netc++"}});
    CHECK(r.ok());
    CHECK(r.body.find("netc++") != std::string::npos);
}

ONLINE_TEST(online_tls_socket) {
    auto s = netcpp::tls::connect("example.com", 443);
    s->send("HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    CHECK(s->read_line()->find("HTTP/1.1") == 0);
}

ONLINE_TEST(online_wss_echo) {
    auto ws = netcpp::ws::connect("wss://ws.postman-echo.com/raw");
    ws->set_timeout(10s);
    ws->send("netc++");
    auto m = ws->receive();
    CHECK(m && m->data == "netc++");
    ws->close();
}

ONLINE_TEST(online_public_ip) { CHECK(!netcpp::util::public_ip().empty()); }

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    const char* online_env = std::getenv("NETCPP_ONLINE");
    bool online = online_env && std::string(online_env) == "1";
    std::string filter = argc > 1 ? argv[1] : "";
    int passed = 0, failed = 0, skipped = 0;
    for (const auto& t : registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
        if (t.online && !online) {
            ++skipped;
            continue;
        }
        auto start = std::chrono::steady_clock::now();
        try {
            t.fn();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            std::cout << "[PASS] " << t.name << " (" << ms << " ms)\n";
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << t.name << ": " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << skipped
              << " skipped (set NETCPP_ONLINE=1 for online tests)\n";
    return failed == 0 ? 0 : 1;
}
