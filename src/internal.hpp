#pragma once
// Internal helpers shared between translation units.

#include "netcpp/socket.hpp"

#include <string>

namespace netcpp::detail {

/// curl_global_init exactly once per process.
void ensure_curl();

/// Opens a raw byte stream through libcurl's CONNECT_ONLY mode.
/// `url` is "http://host:port/" (plain TCP) or "https://host:port/" (TLS).
TlsSocket::Ptr open_curl_stream(const std::string& url, bool verify, const std::string& ca_bundle,
                                Milliseconds timeout);

}  // namespace netcpp::detail
