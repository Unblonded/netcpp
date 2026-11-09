#pragma once

#include <stdexcept>
#include <string>

namespace netcpp {

/// Broad category of a failure, so callers can react without parsing messages.
enum class ErrorKind {
    Network,          ///< connection refused/reset, socket errors, generic transfer errors
    Timeout,          ///< an operation exceeded its deadline
    Dns,              ///< host name could not be resolved
    Tls,              ///< TLS handshake / certificate problems
    Http,             ///< HTTP status error (raised by Response::raise_for_status)
    Protocol,         ///< malformed data from the peer (e.g. bad WebSocket frame)
    Closed,           ///< the connection was closed
    Aborted,          ///< a user callback asked to stop the transfer
    InvalidArgument,  ///< bad URL, bad parameter
    Io                ///< local file problems
};

inline const char* to_string(ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::Network: return "network";
        case ErrorKind::Timeout: return "timeout";
        case ErrorKind::Dns: return "dns";
        case ErrorKind::Tls: return "tls";
        case ErrorKind::Http: return "http";
        case ErrorKind::Protocol: return "protocol";
        case ErrorKind::Closed: return "closed";
        case ErrorKind::Aborted: return "aborted";
        case ErrorKind::InvalidArgument: return "invalid_argument";
        case ErrorKind::Io: return "io";
    }
    return "unknown";
}

/// The single exception type thrown by netc++.
class Error : public std::runtime_error {
public:
    Error(ErrorKind kind, const std::string& message, int code = 0)
        : std::runtime_error(message), kind_(kind), code_(code) {}

    ErrorKind kind() const noexcept { return kind_; }
    /// OS error, curl code or HTTP status depending on kind(); 0 when not applicable.
    int code() const noexcept { return code_; }
    bool is_timeout() const noexcept { return kind_ == ErrorKind::Timeout; }

private:
    ErrorKind kind_;
    int code_;
};

}  // namespace netcpp
