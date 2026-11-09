#pragma once
// Internal: thin portability layer over Winsock / BSD sockets.

#include "netcpp/error.hpp"

#include <chrono>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace netcpp::detail {

#ifdef _WIN32
using sock_t = SOCKET;
const sock_t kInvalidSocket = INVALID_SOCKET;
using socklen = int;
#else
using sock_t = int;
const sock_t kInvalidSocket = -1;
using socklen = socklen_t;
#endif

inline sock_t to_sock(std::uintptr_t h) { return static_cast<sock_t>(h); }
inline std::uintptr_t from_sock(sock_t s) { return static_cast<std::uintptr_t>(s); }
inline std::uintptr_t invalid_handle() { return from_sock(kInvalidSocket); }

/// WSAStartup on Windows (once); no-op elsewhere.
void net_init();
int last_error();
std::string error_message(int code);
bool would_block(int code);
bool interrupted(int code);
void close_socket(sock_t s);
void set_nonblocking(sock_t s, bool enabled);
/// Applies per-platform defaults (no SIGPIPE on macOS, ...).
void configure_socket(sock_t s);
/// >0 ready, 0 timeout. timeout_ms < 0 waits forever. Throws on error.
int wait_socket(sock_t s, bool for_read, bool for_write, int timeout_ms);
[[noreturn]] void throw_os(ErrorKind kind, const std::string& what, int code);

std::string sockaddr_to_string(const sockaddr* sa, std::uint16_t* port_out = nullptr);

struct AddrInfo {
    addrinfo* list = nullptr;
    AddrInfo() = default;
    AddrInfo(const AddrInfo&) = delete;
    AddrInfo& operator=(const AddrInfo&) = delete;
    ~AddrInfo() {
        if (list) freeaddrinfo(list);
    }
};
/// getaddrinfo wrapper; throws Error(Dns).
addrinfo* resolve(const std::string& host, std::uint16_t port, int family, int socktype, int flags,
                  AddrInfo& holder);

inline int to_timeout_ms(std::chrono::milliseconds ms) {
    if (ms.count() <= 0) return -1;
    if (ms.count() > 0x7fffffff) return 0x7fffffff;
    return static_cast<int>(ms.count());
}

}  // namespace netcpp::detail
