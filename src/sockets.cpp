#include "netcpp/socket.hpp"

#include "platform.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

namespace netcpp {

using namespace detail;

// ---------------------------------------------------------------------------
// Stream helpers
// ---------------------------------------------------------------------------
int Stream::timeout_ms() const { return to_timeout_ms(timeout_); }

void Stream::send(std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) off += write_some(data.data() + off, data.size() - off);
}

std::string Stream::receive(std::size_t max_bytes) {
    if (max_bytes == 0) return {};
    if (!buffer_.empty()) {
        std::size_t n = std::min(max_bytes, buffer_.size());
        std::string out = buffer_.substr(0, n);
        buffer_.erase(0, n);
        return out;
    }
    std::string out(max_bytes, '\0');
    std::size_t n = read_some(&out[0], max_bytes);
    out.resize(n);
    return out;
}

std::string Stream::receive_exact(std::size_t n) {
    std::string out;
    out.reserve(n);
    std::size_t take = std::min(n, buffer_.size());
    out.append(buffer_, 0, take);
    buffer_.erase(0, take);
    char tmp[16384];
    while (out.size() < n) {
        std::size_t want = std::min(sizeof(tmp), n - out.size());
        std::size_t got = read_some(tmp, want);
        if (got == 0) throw Error(ErrorKind::Closed, "connection closed before all bytes arrived");
        out.append(tmp, got);
    }
    return out;
}

std::string Stream::receive_until(std::string_view delimiter, std::size_t max_bytes) {
    if (delimiter.empty()) throw Error(ErrorKind::InvalidArgument, "empty delimiter");
    std::size_t search_from = 0;
    char tmp[16384];
    for (;;) {
        std::size_t pos = buffer_.find(delimiter.data(), search_from, delimiter.size());
        if (pos != std::string::npos) {
            std::size_t end = pos + delimiter.size();
            std::string out = buffer_.substr(0, end);
            buffer_.erase(0, end);
            return out;
        }
        if (buffer_.size() > max_bytes) {
            throw Error(ErrorKind::Protocol, "delimiter not found within size limit");
        }
        search_from = buffer_.size() >= delimiter.size() ? buffer_.size() - delimiter.size() + 1 : 0;
        std::size_t got = read_some(tmp, sizeof(tmp));
        if (got == 0) {
            std::string out = std::move(buffer_);
            buffer_.clear();
            return out;
        }
        buffer_.append(tmp, got);
    }
}

std::optional<std::string> Stream::read_line() {
    std::string line = receive_until("\n");
    if (line.empty()) return std::nullopt;
    if (!line.empty() && line.back() == '\n') line.pop_back();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::string Stream::receive_all() {
    std::string out = std::move(buffer_);
    buffer_.clear();
    char tmp[16384];
    while (std::size_t got = read_some(tmp, sizeof(tmp))) out.append(tmp, got);
    return out;
}

// ---------------------------------------------------------------------------
// TcpSocket
// ---------------------------------------------------------------------------
namespace {
std::string address_of(sock_t s, bool peer, std::uint16_t* port) {
    sockaddr_storage ss{};
    socklen len = sizeof(ss);
    int rc = peer ? ::getpeername(s, reinterpret_cast<sockaddr*>(&ss), &len)
                  : ::getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len);
    if (rc != 0) throw_os(ErrorKind::Network, peer ? "getpeername" : "getsockname", last_error());
    return sockaddr_to_string(reinterpret_cast<sockaddr*>(&ss), port);
}

int send_flags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}
}  // namespace

TcpSocket::TcpSocket(native_socket handle) : handle_(handle) {}

TcpSocket::~TcpSocket() { close(); }

void TcpSocket::close() {
    if (handle_ != invalid_handle()) {
        close_socket(to_sock(handle_));
        handle_ = invalid_handle();
    }
}

bool TcpSocket::is_open() const { return handle_ != invalid_handle(); }

void TcpSocket::shutdown_send() {
    if (!is_open()) return;
#ifdef _WIN32
    ::shutdown(to_sock(handle_), SD_SEND);
#else
    ::shutdown(to_sock(handle_), SHUT_WR);
#endif
}

std::string TcpSocket::remote_address() const { return address_of(to_sock(handle_), true, nullptr); }
std::uint16_t TcpSocket::remote_port() const {
    std::uint16_t p = 0;
    address_of(to_sock(handle_), true, &p);
    return p;
}
std::string TcpSocket::local_address() const { return address_of(to_sock(handle_), false, nullptr); }
std::uint16_t TcpSocket::local_port() const {
    std::uint16_t p = 0;
    address_of(to_sock(handle_), false, &p);
    return p;
}

void TcpSocket::set_no_delay(bool enabled) {
    int v = enabled ? 1 : 0;
    ::setsockopt(to_sock(handle_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v),
                 sizeof(v));
}

void TcpSocket::set_keep_alive(bool enabled) {
    int v = enabled ? 1 : 0;
    ::setsockopt(to_sock(handle_), SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&v),
                 sizeof(v));
}

std::size_t TcpSocket::write_some(const char* data, std::size_t len) {
    if (!is_open()) throw Error(ErrorKind::Closed, "socket is closed");
    sock_t s = to_sock(handle_);
    for (;;) {
#ifdef _WIN32
        int n = ::send(s, data, static_cast<int>(std::min<std::size_t>(len, INT_MAX)), 0);
#else
        ssize_t n = ::send(s, data, len, send_flags());
#endif
        if (n >= 0) return static_cast<std::size_t>(n);
        int err = last_error();
        if (interrupted(err)) continue;
        if (!would_block(err)) throw_os(ErrorKind::Network, "send failed", err);
        if (wait_socket(s, false, true, timeout_ms()) == 0) {
            throw Error(ErrorKind::Timeout, "send timed out");
        }
    }
}

std::size_t TcpSocket::read_some(char* buffer, std::size_t len) {
    if (!is_open()) throw Error(ErrorKind::Closed, "socket is closed");
    sock_t s = to_sock(handle_);
    for (;;) {
#ifdef _WIN32
        int n = ::recv(s, buffer, static_cast<int>(std::min<std::size_t>(len, INT_MAX)), 0);
#else
        ssize_t n = ::recv(s, buffer, len, 0);
#endif
        if (n >= 0) return static_cast<std::size_t>(n);
        int err = last_error();
        if (interrupted(err)) continue;
        if (!would_block(err)) throw_os(ErrorKind::Network, "recv failed", err);
        if (wait_socket(s, true, false, timeout_ms()) == 0) {
            throw Error(ErrorKind::Timeout, "receive timed out");
        }
    }
}

// ---------------------------------------------------------------------------
// TcpServer
// ---------------------------------------------------------------------------
TcpServer::TcpServer(native_socket handle) : handle_(handle) {}
TcpServer::~TcpServer() { close(); }

void TcpServer::close() {
    if (handle_ != invalid_handle()) {
        close_socket(to_sock(handle_));
        handle_ = invalid_handle();
    }
}

bool TcpServer::is_open() const { return handle_ != invalid_handle(); }

std::uint16_t TcpServer::port() const {
    std::uint16_t p = 0;
    address_of(to_sock(handle_), false, &p);
    return p;
}

std::string TcpServer::address() const { return address_of(to_sock(handle_), false, nullptr); }

TcpSocket::Ptr TcpServer::accept(std::optional<Milliseconds> timeout) {
    if (!is_open()) throw Error(ErrorKind::Closed, "server is closed");
    sock_t s = to_sock(handle_);
    int wait_ms = timeout ? to_timeout_ms(*timeout) : -1;
    if (timeout && timeout->count() <= 0) wait_ms = 0;
    for (;;) {
        if (wait_socket(s, true, false, wait_ms) == 0) return nullptr;
        sockaddr_storage ss{};
        socklen len = sizeof(ss);
        sock_t c = ::accept(s, reinterpret_cast<sockaddr*>(&ss), &len);
        if (c == kInvalidSocket) {
            int err = last_error();
            if (interrupted(err) || would_block(err)) continue;
            throw_os(ErrorKind::Network, "accept failed", err);
        }
        configure_socket(c);
        set_nonblocking(c, true);
        return std::make_shared<TcpSocket>(from_sock(c));
    }
}

namespace tcp {

TcpSocket::Ptr connect(const std::string& host, std::uint16_t port, Milliseconds timeout) {
    AddrInfo holder;
    addrinfo* list = resolve(host, port, AF_UNSPEC, SOCK_STREAM, 0, holder);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    int last = 0;
    bool timed_out = false;
    const std::string where = host + ":" + std::to_string(port);

    for (addrinfo* p = list; p; p = p->ai_next) {
        sock_t s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == kInvalidSocket) {
            last = last_error();
            continue;
        }
        configure_socket(s);
        set_nonblocking(s, true);
        if (::connect(s, p->ai_addr, static_cast<socklen>(p->ai_addrlen)) != 0) {
            int err = last_error();
            if (!would_block(err)) {
                close_socket(s);
                last = err;
                continue;
            }
            auto left = std::chrono::duration_cast<Milliseconds>(deadline -
                                                                 std::chrono::steady_clock::now());
            if (timeout.count() > 0 && left.count() <= 0) {
                close_socket(s);
                timed_out = true;
                break;
            }
            int ready = wait_socket(s, false, true, timeout.count() > 0 ? to_timeout_ms(left) : -1);
            if (ready == 0) {
                close_socket(s);
                timed_out = true;
                continue;
            }
            int so_error = 0;
            socklen len = sizeof(so_error);
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
            if (so_error != 0) {
                close_socket(s);
                last = so_error;
                continue;
            }
        }
        return std::make_shared<TcpSocket>(from_sock(s));
    }
    if (timed_out) throw Error(ErrorKind::Timeout, "connect to " + where + " timed out");
    throw_os(ErrorKind::Network, "connect to " + where + " failed", last);
}

TcpServer::Ptr listen(std::uint16_t port, const std::string& host, int backlog) {
    AddrInfo holder;
    addrinfo* list = resolve(host, port, AF_UNSPEC, SOCK_STREAM, AI_PASSIVE, holder);
    int last = 0;
    for (addrinfo* p = list; p; p = p->ai_next) {
        sock_t s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == kInvalidSocket) {
            last = last_error();
            continue;
        }
#ifndef _WIN32
        int one = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
        if (p->ai_family == AF_INET6) {
            int zero = 0;  // accept IPv4 too when bound to "::"
            ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&zero),
                         sizeof(zero));
        }
        if (::bind(s, p->ai_addr, static_cast<socklen>(p->ai_addrlen)) != 0 ||
            ::listen(s, backlog) != 0) {
            last = last_error();
            close_socket(s);
            continue;
        }
        set_nonblocking(s, true);
        return std::make_shared<TcpServer>(from_sock(s));
    }
    throw_os(ErrorKind::Network, "listen on " + host + ":" + std::to_string(port) + " failed", last);
}

}  // namespace tcp

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------
UdpSocket::UdpSocket(native_socket handle, int family) : handle_(handle), family_(family) {}
UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
    if (handle_ != invalid_handle()) {
        close_socket(to_sock(handle_));
        handle_ = invalid_handle();
    }
}

bool UdpSocket::is_open() const { return handle_ != invalid_handle(); }

std::uint16_t UdpSocket::local_port() const {
    std::uint16_t p = 0;
    address_of(to_sock(handle_), false, &p);
    return p;
}

void UdpSocket::set_broadcast(bool enabled) {
    int v = enabled ? 1 : 0;
    ::setsockopt(to_sock(handle_), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&v),
                 sizeof(v));
}

std::size_t UdpSocket::send_to(const std::string& host, std::uint16_t port, std::string_view data) {
    if (!is_open()) throw Error(ErrorKind::Closed, "socket is closed");
    AddrInfo holder;
    addrinfo* p = resolve(host, port, family_, SOCK_DGRAM, 0, holder);
    sock_t s = to_sock(handle_);
    for (;;) {
#ifdef _WIN32
        int n = ::sendto(s, data.data(), static_cast<int>(data.size()), 0, p->ai_addr,
                         static_cast<socklen>(p->ai_addrlen));
#else
        ssize_t n = ::sendto(s, data.data(), data.size(), send_flags(), p->ai_addr,
                             static_cast<socklen>(p->ai_addrlen));
#endif
        if (n >= 0) return static_cast<std::size_t>(n);
        int err = last_error();
        if (interrupted(err)) continue;
        if (!would_block(err)) throw_os(ErrorKind::Network, "sendto failed", err);
        wait_socket(s, false, true, 5000);
    }
}

std::optional<Datagram> UdpSocket::receive_from(Milliseconds timeout, std::size_t max_bytes) {
    if (!is_open()) throw Error(ErrorKind::Closed, "socket is closed");
    sock_t s = to_sock(handle_);
    std::string buf(max_bytes, '\0');
    for (;;) {
        if (wait_socket(s, true, false, to_timeout_ms(timeout)) == 0) return std::nullopt;
        sockaddr_storage ss{};
        socklen len = sizeof(ss);
#ifdef _WIN32
        int n = ::recvfrom(s, &buf[0], static_cast<int>(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&ss), &len);
#else
        ssize_t n = ::recvfrom(s, &buf[0], buf.size(), 0, reinterpret_cast<sockaddr*>(&ss), &len);
#endif
        if (n < 0) {
            int err = last_error();
            if (interrupted(err) || would_block(err)) continue;
#ifdef _WIN32
            if (err == WSAECONNRESET || err == WSAEMSGSIZE) continue;  // ICMP noise / truncation
#endif
            throw_os(ErrorKind::Network, "recvfrom failed", err);
        }
        Datagram d;
        d.data.assign(buf.data(), static_cast<std::size_t>(n));
        d.address = sockaddr_to_string(reinterpret_cast<sockaddr*>(&ss), &d.port);
        return d;
    }
}

namespace udp {

UdpSocket::Ptr open(std::uint16_t port, const std::string& host) {
    AddrInfo holder;
    addrinfo* list = resolve(host, port, AF_UNSPEC, SOCK_DGRAM, AI_PASSIVE, holder);
    int last = 0;
    for (addrinfo* p = list; p; p = p->ai_next) {
        sock_t s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == kInvalidSocket) {
            last = last_error();
            continue;
        }
        if (::bind(s, p->ai_addr, static_cast<socklen>(p->ai_addrlen)) != 0) {
            last = last_error();
            close_socket(s);
            continue;
        }
        set_nonblocking(s, true);
        return std::make_shared<UdpSocket>(from_sock(s), p->ai_family);
    }
    throw_os(ErrorKind::Network, "udp bind on " + host + ":" + std::to_string(port) + " failed",
             last);
}

}  // namespace udp

}  // namespace netcpp
