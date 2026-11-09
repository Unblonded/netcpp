#include "internal.hpp"
#include "platform.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace netcpp {

namespace detail {

void ensure_curl() {
    static std::once_flag once;
    std::call_once(once, [] {
        CURLcode rc = curl_global_init(CURL_GLOBAL_ALL);
        if (rc != CURLE_OK) {
            throw Error(ErrorKind::Network, std::string("curl_global_init failed: ") +
                                                curl_easy_strerror(rc),
                        static_cast<int>(rc));
        }
        std::atexit([] { curl_global_cleanup(); });
    });
}

}  // namespace detail

/// libcurl easy handle in CONNECT_ONLY mode: a raw (optionally TLS) byte pipe.
struct TlsSocket::Impl {
    CURL* handle = nullptr;
    curl_socket_t sock = CURL_SOCKET_BAD;
    std::mutex io;
    std::atomic<bool> closed{false};

    ~Impl() { shutdown(); }

    void shutdown() {
        std::lock_guard<std::mutex> lock(io);
        closed = true;
        if (handle) {
            curl_easy_cleanup(handle);
            handle = nullptr;
        }
    }

    // Waits in short slices so a concurrent close() is noticed.
    void wait(bool for_read, int timeout_ms) {
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (closed) throw Error(ErrorKind::Closed, "connection closed");
            int slice = 200;
            if (timeout_ms >= 0) {
                auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
                int left = timeout_ms - static_cast<int>(spent);
                if (left <= 0) {
                    throw Error(ErrorKind::Timeout, for_read ? "receive timed out" : "send timed out");
                }
                slice = std::min(slice, left);
            }
            if (detail::wait_socket(static_cast<detail::sock_t>(sock), for_read, !for_read, slice) > 0) {
                return;
            }
        }
    }

    std::size_t write_some(const char* data, std::size_t len, int timeout_ms) {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(io);
                if (!handle) throw Error(ErrorKind::Closed, "connection closed");
                std::size_t sent = 0;
                CURLcode rc = curl_easy_send(handle, data, len, &sent);
                if (rc == CURLE_OK) return sent;
                if (rc != CURLE_AGAIN) {
                    throw Error(ErrorKind::Network, std::string("send failed: ") + curl_easy_strerror(rc),
                                static_cast<int>(rc));
                }
            }
            wait(false, timeout_ms);
        }
    }

    std::size_t read_some(char* buffer, std::size_t len, int timeout_ms) {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(io);
                if (!handle) throw Error(ErrorKind::Closed, "connection closed");
                std::size_t got = 0;
                CURLcode rc = curl_easy_recv(handle, buffer, len, &got);
                if (rc == CURLE_OK) return got;  // 0 == peer closed
                if (rc != CURLE_AGAIN) {
                    throw Error(ErrorKind::Network,
                                std::string("receive failed: ") + curl_easy_strerror(rc),
                                static_cast<int>(rc));
                }
            }
            wait(true, timeout_ms);
        }
    }
};

TlsSocket::TlsSocket(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TlsSocket::~TlsSocket() = default;

void TlsSocket::close() { impl_->shutdown(); }
bool TlsSocket::is_open() const { return !impl_->closed; }

std::size_t TlsSocket::write_some(const char* data, std::size_t len) {
    return impl_->write_some(data, len, timeout_ms());
}

std::size_t TlsSocket::read_some(char* buffer, std::size_t len) {
    return impl_->read_some(buffer, len, timeout_ms());
}

namespace detail {

TlsSocket::Ptr open_curl_stream(const std::string& url, bool verify, const std::string& ca_bundle,
                                Milliseconds timeout) {
    ensure_curl();
    auto impl = std::make_unique<TlsSocket::Impl>();
    impl->handle = curl_easy_init();
    if (!impl->handle) throw Error(ErrorKind::Network, "curl_easy_init failed");
    CURL* h = impl->handle;
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_NOPROXY, "*");
    // Raw byte stream: never let ALPN negotiate HTTP/2 on this connection.
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    curl_easy_setopt(h, CURLOPT_SSL_ENABLE_ALPN, 0L);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    if (timeout.count() > 0) {
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout.count()));
    }
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);
    if (!ca_bundle.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, ca_bundle.c_str());

    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        std::string msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        ErrorKind kind = ErrorKind::Network;
        if (rc == CURLE_OPERATION_TIMEDOUT) kind = ErrorKind::Timeout;
        else if (rc == CURLE_COULDNT_RESOLVE_HOST) kind = ErrorKind::Dns;
        else if (rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_PEER_FAILED_VERIFICATION) kind = ErrorKind::Tls;
        throw Error(kind, "connect to " + url + " failed: " + msg, static_cast<int>(rc));
    }
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, nullptr);
    curl_socket_t sock = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(h, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK || sock == CURL_SOCKET_BAD) {
        throw Error(ErrorKind::Network, "could not obtain socket for " + url);
    }
    impl->sock = sock;
    return std::make_shared<TlsSocket>(std::move(impl));
}

}  // namespace detail

namespace tls {

TlsSocket::Ptr connect(const std::string& host, std::uint16_t port, const Options& options) {
    std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    auto s = detail::open_curl_stream("https://" + h + ":" + std::to_string(port) + "/",
                                      options.verify, options.ca_bundle, options.timeout);
    s->set_timeout(options.timeout);
    return s;
}

}  // namespace tls

}  // namespace netcpp
