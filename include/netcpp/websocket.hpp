#pragma once

#include "netcpp/http.hpp"
#include "netcpp/socket.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace netcpp {

/// RFC 6455 WebSocket client. send() is thread-safe; call receive() from one thread.
class WebSocket {
public:
    using Ptr = std::shared_ptr<WebSocket>;

    enum class MessageType { Text, Binary };
    struct Message {
        MessageType type = MessageType::Text;
        std::string data;
        bool is_text() const { return type == MessageType::Text; }
    };

    /// Wraps an already-upgraded stream (use ws::connect normally).
    WebSocket(std::shared_ptr<Stream> stream, std::string protocol = "");
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    void send(std::string_view text);
    void send_binary(std::string_view data);
    void ping(std::string_view payload = {});

    /// Next text/binary message. Pings are answered automatically.
    /// Returns nullopt once the connection is closed.
    std::optional<Message> receive();

    /// Starts the closing handshake; receive() then returns nullopt.
    void close(std::uint16_t code = 1000, std::string_view reason = {});
    bool is_open() const { return open_; }

    /// Timeout for receive(); 0 = wait forever (default).
    void set_timeout(Milliseconds timeout);
    void set_max_message_size(std::size_t bytes) { max_message_size_ = bytes; }

    const std::string& protocol() const { return protocol_; }
    std::uint16_t close_code() const { return close_code_; }
    const std::string& close_reason() const { return close_reason_; }

private:
    void send_frame(std::uint8_t opcode, std::string_view payload);

    std::shared_ptr<Stream> stream_;
    std::string protocol_;
    std::mutex send_mutex_;
    std::atomic<bool> open_{true};
    std::atomic<bool> close_sent_{false};
    std::uint16_t close_code_ = 0;
    std::string close_reason_;
    std::size_t max_message_size_ = 64 * 1024 * 1024;
};

namespace ws {
struct Options {
    http::Headers headers;
    std::vector<std::string> protocols;  ///< Sec-WebSocket-Protocol candidates
    Milliseconds timeout{10000};         ///< connect + handshake timeout
    bool verify_tls = true;
    std::string ca_bundle;
};

/// Connects to ws:// or wss:// and performs the opening handshake.
WebSocket::Ptr connect(const std::string& url, const Options& options = {});
}  // namespace ws

}  // namespace netcpp
