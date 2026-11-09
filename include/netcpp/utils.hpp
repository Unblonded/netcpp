#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace netcpp::util {

using Milliseconds = std::chrono::milliseconds;

/// This machine's host name.
std::string hostname();

struct Interface {
    std::string name;
    std::string address;
    bool ipv6 = false;
    bool loopback = false;
    bool up = false;
};
/// Every IP address assigned to a local network interface.
std::vector<Interface> interfaces();

/// True if a TCP connection to host:port succeeds within `timeout`.
bool is_port_open(const std::string& host, std::uint16_t port,
                  Milliseconds timeout = Milliseconds(1000));
/// Returns the open ports among `ports` (sorted), probing `concurrency` at a time.
std::vector<std::uint16_t> scan_ports(const std::string& host,
                                      const std::vector<std::uint16_t>& ports,
                                      Milliseconds timeout = Milliseconds(500),
                                      unsigned concurrency = 64);
/// Inclusive range helper for scan_ports: port_range(1, 1024).
std::vector<std::uint16_t> port_range(std::uint16_t first, std::uint16_t last);
/// TCP connect round-trip time in milliseconds (works without root, unlike ICMP).
std::optional<double> tcp_ping(const std::string& host, std::uint16_t port = 80,
                               Milliseconds timeout = Milliseconds(2000));
/// Polls until host:port accepts connections or `total` elapses.
bool wait_for_port(const std::string& host, std::uint16_t port, Milliseconds total,
                   Milliseconds interval = Milliseconds(100));
/// Public IP as seen from the internet (queries api.ipify.org).
std::string public_ip(Milliseconds timeout = Milliseconds(5000));

bool is_ipv4(std::string_view text);
bool is_ipv6(std::string_view text);

/// "netc++ 1.0.0 (libcurl/x.y.z ...)"
std::string version();

}  // namespace netcpp::util
