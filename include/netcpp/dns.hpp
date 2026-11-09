#pragma once

#include <string>
#include <vector>

namespace netcpp::dns {

enum class Family { Any, IPv4, IPv6 };

/// All addresses for `host` (deduplicated, resolver order). Throws Error(Dns).
std::vector<std::string> resolve(const std::string& host, Family family = Family::Any);
/// First address for `host`. Throws Error(Dns) when none.
std::string resolve_one(const std::string& host, Family family = Family::Any);
/// Reverse lookup (PTR) of an IP address. Throws Error(Dns).
std::string reverse(const std::string& ip);

}  // namespace netcpp::dns
