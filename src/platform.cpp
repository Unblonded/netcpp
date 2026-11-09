#include "platform.hpp"

#include <mutex>
#include <system_error>

namespace netcpp::detail {

void net_init() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) throw Error(ErrorKind::Network, "WSAStartup failed", rc);
    });
#endif
}

int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string error_message(int code) {
    std::string msg = std::system_category().message(code);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ' ||
                            msg.back() == '.')) {
        msg.pop_back();
    }
    return msg;
}

bool would_block(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == EINPROGRESS;
#endif
}

bool interrupted(int code) {
#ifdef _WIN32
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

void close_socket(sock_t s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

void set_nonblocking(sock_t s, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    if (::ioctlsocket(s, FIONBIO, &mode) != 0) throw_os(ErrorKind::Network, "ioctlsocket", last_error());
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) throw_os(ErrorKind::Network, "fcntl", last_error());
    flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(s, F_SETFL, flags) < 0) throw_os(ErrorKind::Network, "fcntl", last_error());
#endif
}

void configure_socket(sock_t s) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    (void)s;
#endif
}

int wait_socket(sock_t s, bool for_read, bool for_write, int timeout_ms) {
#ifdef _WIN32
    // select() (not WSAPoll) so failed non-blocking connects are reported reliably.
    fd_set rs, ws, es;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    FD_ZERO(&es);
    if (for_read) FD_SET(s, &rs);
    if (for_write) {
        FD_SET(s, &ws);
        FD_SET(s, &es);
    }
    timeval tv{};
    timeval* ptv = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int rc = ::select(0, for_read ? &rs : nullptr, for_write ? &ws : nullptr,
                      for_write ? &es : nullptr, ptv);
    if (rc == SOCKET_ERROR) throw_os(ErrorKind::Network, "select", last_error());
    return rc;
#else
    pollfd p{};
    p.fd = s;
    p.events = static_cast<short>((for_read ? POLLIN : 0) | (for_write ? POLLOUT : 0));
    for (;;) {
        int rc = ::poll(&p, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            throw_os(ErrorKind::Network, "poll", errno);
        }
        return rc;
    }
#endif
}

void throw_os(ErrorKind kind, const std::string& what, int code) {
    throw Error(kind, what + ": " + error_message(code), code);
}

std::string sockaddr_to_string(const sockaddr* sa, std::uint16_t* port_out) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (sa->sa_family == AF_INET) {
        auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        ::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf));
        if (port_out) *port_out = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        ::inet_ntop(AF_INET6, &in6->sin6_addr, buf, sizeof(buf));
        if (port_out) *port_out = ntohs(in6->sin6_port);
    } else {
        if (port_out) *port_out = 0;
    }
    return buf;
}

addrinfo* resolve(const std::string& host, std::uint16_t port, int family, int socktype, int flags,
                  AddrInfo& holder) {
    net_init();
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_flags = flags;
    std::string service = std::to_string(port);
    const char* node = host.empty() ? nullptr : host.c_str();
    int rc = ::getaddrinfo(node, service.c_str(), &hints, &holder.list);
    if (rc != 0 || holder.list == nullptr) {
#ifdef _WIN32
        std::string reason = error_message(rc);
#else
        std::string reason = rc == EAI_SYSTEM ? error_message(errno) : ::gai_strerror(rc);
#endif
        throw Error(ErrorKind::Dns, "could not resolve '" + host + "': " + reason, rc);
    }
    return holder.list;
}

}  // namespace netcpp::detail
