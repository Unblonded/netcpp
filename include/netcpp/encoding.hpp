#pragma once

#include <string>
#include <string_view>

namespace netcpp {

/// Standard Base64 (RFC 4648) with padding.
std::string base64_encode(std::string_view data);
/// Decodes Base64, ignoring whitespace. Throws Error(InvalidArgument) on bad input.
std::string base64_decode(std::string_view data);

/// Percent-encodes everything except RFC 3986 unreserved characters.
std::string url_encode(std::string_view text);
/// Decodes %XX sequences; optionally turns '+' into a space (form encoding).
std::string url_decode(std::string_view text, bool plus_as_space = true);

/// Raw 20-byte SHA-1 digest.
std::string sha1(std::string_view data);
/// Lower-case hexadecimal representation of arbitrary bytes.
std::string hex_encode(std::string_view data);

}  // namespace netcpp
