#pragma once

#include "netcpp/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace netcpp {

using Milliseconds = std::chrono::milliseconds;
/// Platform socket handle (SOCKET on Windows, int on POSIX) stored portably.
using native_socket = std::uintptr_t;

// ---------------------------------------------------------------------------
// Stream: common byte-stream API for TCP, TLS and WebSocket transports
// ---------------------------------------------------------------------------
class Stream {
public:
    virtual ~Stream() = default;

    /// Sends every byte (loops over partial writes).
    void send(std::string_view data);
    /// Returns up to `max_bytes` (at least 1); empty string means the peer closed.
    std::string receive(std::size_t max_bytes = 65536);
    /// Exactly `n` bytes, or throws Error(Closed).
    std::string receive_exact(std::size_t n);
    /// Reads up to and including `delimiter`. At EOF returns whatever was left.
    std::string receive_until(std::string_view delimiter, std::size_t max_bytes = 1 << 20);
    /// One line without "\r\n"/"\n"; nullopt at EOF.
    std::optional<std::string> read_line();
    /// Reads until the peer closes the connection.
    std::string receive_all();

    /// Per-operation timeout; 0 disables it. Timeouts throw Error(Timeout).
    void set_timeout(Milliseconds timeout) { timeout_ = timeout; }
    Milliseconds timeout() const { return timeout_; }

    virtual void close() = 0;
    virtual bool is_open() const = 0;

protected:
    virtual std::size_t write_some(const char* data, std::size_t len) = 0;
    /// Returns 0 when the peer closed the connection.
    virtual std::size_t read_some(char* buffer, std::size_t len) = 0;
    int timeout_ms() const;

private:
    Milliseconds timeout_{30000};
    std::string buffer_;
};

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------
class TcpSocket : public Stream {
public:
    using Ptr = std::shared_ptr<TcpSocket>;

    /// Takes ownership of an already connected socket handle.
    explicit TcpSocket(native_socket handle);
    ~TcpSocket() override;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    void close() override;
    bool is_open() const override;
    /// Half-close: tell the peer we are done sending.
    void shutdown_send();

    std::string remote_address() const;
    std::uint16_t remote_port() const;
    std::string local_address() const;
    std::uint16_t local_port() const;

    void set_no_delay(bool enabled = true);
    void set_keep_alive(bool enabled = true);
    native_socket native_handle() const { return handle_; }

protected:
    std::size_t write_some(const char* data, std::size_t len) override;
    std::size_t read_some(char* buffer, std::size_t len) override;

private:
    native_socket handle_;
};

class TcpServer {
public:
    using Ptr = std::shared_ptr<TcpServer>;

    explicit TcpServer(native_socket handle);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /// Waits for a client. Returns nullptr if `timeout` elapses first.
    TcpSocket::Ptr accept(std::optional<Milliseconds> timeout = std::nullopt);
    /// The bound port (useful after listening on port 0).
    std::uint16_t port() const;
    std::string address() const;
    void close();
    bool is_open() const;
    native_socket native_handle() const { return handle_; }

private:
    native_socket handle_;
};

// ---------------------------------------------------------------------------
// TLS client stream (backed by libcurl's TLS stack: Schannel / SecureTransport / OpenSSL)
// ---------------------------------------------------------------------------
class TlsSocket : public Stream {
public:
    using Ptr = std::shared_ptr<TlsSocket>;
    struct Impl;

    explicit TlsSocket(std::unique_ptr<Impl> impl);
    ~TlsSocket() override;

    void close() override;
    bool is_open() const override;

protected:
    std::size_t write_some(const char* data, std::size_t len) override;
    std::size_t read_some(char* buffer, std::size_t len) override;

private:
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------
struct Datagram {
    std::string data;
    std::string address;
    std::uint16_t port = 0;
};

class UdpSocket {
public:
    using Ptr = std::shared_ptr<UdpSocket>;

    UdpSocket(native_socket handle, int family);
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Returns the number of bytes sent.
    std::size_t send_to(const std::string& host, std::uint16_t port, std::string_view data);
    /// nullopt on timeout (0 = wait forever).
    std::optional<Datagram> receive_from(Milliseconds timeout = Milliseconds(30000),
                                         std::size_t max_bytes = 65535);
    void set_broadcast(bool enabled = true);
    std::uint16_t local_port() const;
    void close();
    bool is_open() const;
    native_socket native_handle() const { return handle_; }

private:
    native_socket handle_;
    int family_;
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
namespace tcp {
/// Connects to host:port (IPv4 or IPv6, whichever answers first in resolver order).
TcpSocket::Ptr connect(const std::string& host, std::uint16_t port,
                       Milliseconds timeout = Milliseconds(10000));
/// Listens on host:port. Port 0 picks a free port (see TcpServer::port()).
TcpServer::Ptr listen(std::uint16_t port, const std::string& host = "0.0.0.0", int backlog = 128);
}  // namespace tcp

namespace tls {
struct Options {
    bool verify = true;
    std::string ca_bundle;
    Milliseconds timeout{10000};
};
TlsSocket::Ptr connect(const std::string& host, std::uint16_t port, const Options& options = {});
}  // namespace tls

namespace udp {
/// Opens a UDP socket bound to host:port (port 0 = any free port).
UdpSocket::Ptr open(std::uint16_t port = 0, const std::string& host = "0.0.0.0");
}  // namespace udp

}  // namespace netcpp
