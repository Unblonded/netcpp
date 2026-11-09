#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace netcpp {

/// Ordered list of key/value pairs (query parameters, form fields).
using Params = std::vector<std::pair<std::string, std::string>>;

/// "a=1&b=two%20words"
std::string build_query(const Params& params);
/// Parses "a=1&b=2" (a leading '?' is ignored).
Params parse_query(std::string_view query);

/// Joins a base URL and a path without doubling or dropping slashes.
std::string join_url(std::string_view base, std::string_view path);

/// A parsed URL: scheme://user:password@host:port/path?query#fragment
struct Url {
    std::string scheme;
    std::string user;
    std::string password;
    std::string host;      ///< without IPv6 brackets
    std::uint16_t port = 0;  ///< 0 when not given explicitly
    std::string path;
    std::string query;     ///< without the leading '?'
    std::string fragment;  ///< without the leading '#'

    /// Throws Error(InvalidArgument) on malformed input.
    static Url parse(std::string_view url);

    /// The explicit port, or the scheme's well-known port (http=80, https=443, ...).
    std::uint16_t effective_port() const;
    /// "host" or "host:port" (IPv6 hosts bracketed); port only if explicit.
    std::string host_port() const;
    /// "/path?query" – what goes on an HTTP request line.
    std::string target() const;
    /// Reassembles the URL.
    std::string str() const;
};

/// Well-known port for a scheme, 0 if unknown.
std::uint16_t default_port(std::string_view scheme);

}  // namespace netcpp
