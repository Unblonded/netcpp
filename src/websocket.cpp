#include "netcpp/websocket.hpp"

#include "internal.hpp"
#include "netcpp/encoding.hpp"
#include "netcpp/url.hpp"

#include <random>

namespace netcpp {

namespace {

enum Opcode : std::uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA
};

const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string random_bytes(std::size_t n) {
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 255);
    std::string out(n, '\0');
    for (auto& c : out) c = static_cast<char>(dist(rng));
    return out;
}

}  // namespace

WebSocket::WebSocket(std::shared_ptr<Stream> stream, std::string protocol)
    : stream_(std::move(stream)), protocol_(std::move(protocol)) {
    stream_->set_timeout(Milliseconds(0));
}

WebSocket::~WebSocket() {
    try {
        if (open_ && !close_sent_) send_frame(kClose, std::string("\x03\xE8", 2));
    } catch (...) {
    }
    open_ = false;
    stream_->close();
}

void WebSocket::set_timeout(Milliseconds timeout) { stream_->set_timeout(timeout); }

void WebSocket::send_frame(std::uint8_t opcode, std::string_view payload) {
    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<char>(0x80 | opcode));
    const std::uint64_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80 | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    const std::string mask = random_bytes(4);
    frame += mask;
    const std::size_t offset = frame.size();
    frame.append(payload.data(), payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) frame[offset + i] ^= mask[i % 4];

    std::lock_guard<std::mutex> lock(send_mutex_);
    stream_->send(frame);
}

void WebSocket::send(std::string_view text) {
    if (!open_ || close_sent_) throw Error(ErrorKind::Closed, "WebSocket is closed");
    send_frame(kText, text);
}

void WebSocket::send_binary(std::string_view data) {
    if (!open_ || close_sent_) throw Error(ErrorKind::Closed, "WebSocket is closed");
    send_frame(kBinary, data);
}

void WebSocket::ping(std::string_view payload) {
    if (payload.size() > 125) throw Error(ErrorKind::InvalidArgument, "ping payload too large");
    if (!open_ || close_sent_) throw Error(ErrorKind::Closed, "WebSocket is closed");
    send_frame(kPing, payload);
}

void WebSocket::close(std::uint16_t code, std::string_view reason) {
    if (!open_ || close_sent_.exchange(true)) return;
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason.data(), std::min<std::size_t>(reason.size(), 123));
    try {
        send_frame(kClose, payload);
    } catch (...) {
        open_ = false;
        stream_->close();
    }
}

std::optional<WebSocket::Message> WebSocket::receive() {
    std::string assembled;
    std::optional<MessageType> type;
    while (open_) {
        std::string header;
        try {
            header = stream_->receive_exact(2);
        } catch (const Error& e) {
            if (e.kind() == ErrorKind::Timeout) throw;
            open_ = false;
            stream_->close();
            return std::nullopt;
        }
        const bool fin = (static_cast<std::uint8_t>(header[0]) & 0x80) != 0;
        const std::uint8_t opcode = static_cast<std::uint8_t>(header[0]) & 0x0F;
        const bool masked = (static_cast<std::uint8_t>(header[1]) & 0x80) != 0;
        std::uint64_t len = static_cast<std::uint8_t>(header[1]) & 0x7F;
        if (len == 126) {
            std::string ext = stream_->receive_exact(2);
            len = (std::uint64_t(std::uint8_t(ext[0])) << 8) | std::uint8_t(ext[1]);
        } else if (len == 127) {
            std::string ext = stream_->receive_exact(8);
            len = 0;
            for (char c : ext) len = (len << 8) | std::uint8_t(c);
        }
        if (len > max_message_size_ || assembled.size() + len > max_message_size_) {
            close(1009, "message too big");
            throw Error(ErrorKind::Protocol, "WebSocket message exceeds size limit");
        }
        std::string mask = masked ? stream_->receive_exact(4) : std::string();
        std::string payload = len ? stream_->receive_exact(static_cast<std::size_t>(len)) : std::string();
        if (masked)
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];

        switch (opcode) {
            case kPing:
                try {
                    if (!close_sent_) send_frame(kPong, payload);
                } catch (...) {
                }
                break;
            case kPong:
                break;
            case kClose: {
                if (payload.size() >= 2) {
                    close_code_ = static_cast<std::uint16_t>((std::uint8_t(payload[0]) << 8) |
                                                             std::uint8_t(payload[1]));
                    close_reason_ = payload.substr(2);
                } else {
                    close_code_ = 1005;
                }
                if (!close_sent_.exchange(true)) {
                    try {
                        send_frame(kClose, payload.substr(0, 2));
                    } catch (...) {
                    }
                }
                open_ = false;
                stream_->close();
                return std::nullopt;
            }
            case kText:
            case kBinary:
                if (type) throw Error(ErrorKind::Protocol, "new message started inside fragmented one");
                type = opcode == kText ? MessageType::Text : MessageType::Binary;
                assembled = std::move(payload);
                if (fin) return Message{*type, std::move(assembled)};
                break;
            case kContinuation:
                if (!type) throw Error(ErrorKind::Protocol, "unexpected continuation frame");
                assembled += payload;
                if (fin) return Message{*type, std::move(assembled)};
                break;
            default:
                close(1002, "unknown opcode");
                throw Error(ErrorKind::Protocol, "unknown WebSocket opcode " + std::to_string(opcode));
        }
    }
    return std::nullopt;
}

namespace ws {

WebSocket::Ptr connect(const std::string& url_text, const Options& options) {
    Url url = Url::parse(url_text);
    bool secure;
    if (url.scheme == "wss" || url.scheme == "https") secure = true;
    else if (url.scheme == "ws" || url.scheme == "http") secure = false;
    else throw Error(ErrorKind::InvalidArgument, "WebSocket URL must be ws:// or wss://");

    const std::uint16_t port = url.port ? url.port : (secure ? 443 : 80);
    std::string host = url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host;
    auto stream = detail::open_curl_stream(
        std::string(secure ? "https://" : "http://") + host + ":" + std::to_string(port) + "/",
        options.verify_tls, options.ca_bundle, options.timeout);
    stream->set_timeout(options.timeout);

    const std::string key = base64_encode(random_bytes(16));
    std::string req = "GET " + url.target() + " HTTP/1.1\r\n";
    req += "Host: " + url.host_port() + "\r\n";
    req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    if (!options.protocols.empty()) {
        std::string joined;
        for (const auto& p : options.protocols) joined += (joined.empty() ? "" : ", ") + p;
        req += "Sec-WebSocket-Protocol: " + joined + "\r\n";
    }
    if (!url.user.empty() && !options.headers.has("Authorization")) {
        req += "Authorization: Basic " + base64_encode(url.user + ":" + url.password) + "\r\n";
    }
    for (const auto& [name, value] : options.headers) req += name + ": " + value + "\r\n";
    req += "\r\n";
    stream->send(req);

    const std::string response = stream->receive_until("\r\n\r\n", 64 * 1024);
    std::size_t line_end = response.find("\r\n");
    std::string status_line = response.substr(0, line_end);
    int status = 0;
    std::size_t sp = status_line.find(' ');
    if (sp != std::string::npos) status = std::atoi(status_line.c_str() + sp + 1);

    http::Headers headers;
    std::size_t pos = line_end == std::string::npos ? response.size() : line_end + 2;
    while (pos < response.size()) {
        std::size_t end = response.find("\r\n", pos);
        if (end == std::string::npos || end == pos) break;
        std::string line = response.substr(pos, end - pos);
        std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(0, 1);
            headers.add(line.substr(0, colon), value);
        }
        pos = end + 2;
    }

    if (status != 101) {
        throw Error(ErrorKind::Protocol,
                    "WebSocket handshake failed: " + (status_line.empty() ? "no response" : status_line),
                    status);
    }
    const std::string expected = base64_encode(sha1(key + kGuid));
    if (headers.get("Sec-WebSocket-Accept") != expected) {
        throw Error(ErrorKind::Protocol, "WebSocket handshake failed: bad Sec-WebSocket-Accept");
    }
    return std::make_shared<WebSocket>(stream, headers.get("Sec-WebSocket-Protocol"));
}

}  // namespace ws

}  // namespace netcpp
