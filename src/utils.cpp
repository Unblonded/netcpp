#include "netcpp/utils.hpp"

#include "internal.hpp"
#include "netcpp/http.hpp"
#include "netcpp/socket.hpp"
#include "platform.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <iphlpapi.h>
#include <vector>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace netcpp::util {

std::string hostname() {
    detail::net_init();
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) {
        detail::throw_os(ErrorKind::Network, "gethostname", detail::last_error());
    }
    return buf;
}

std::vector<Interface> interfaces() {
    detail::net_init();
    std::vector<Interface> out;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        rc = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        if (rc != ERROR_BUFFER_OVERFLOW) break;
        buffer.resize(size);
    }
    if (rc != NO_ERROR) throw Error(ErrorKind::Network, "GetAdaptersAddresses failed", static_cast<int>(rc));
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        std::string name;
        if (a->FriendlyName) {
            int len = ::WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                name.resize(static_cast<std::size_t>(len - 1));
                ::WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, &name[0], len, nullptr, nullptr);
            }
        }
        if (name.empty() && a->AdapterName) name = a->AdapterName;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            Interface itf;
            itf.name = name;
            itf.address = detail::sockaddr_to_string(u->Address.lpSockaddr);
            itf.ipv6 = u->Address.lpSockaddr->sa_family == AF_INET6;
            itf.loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
            itf.up = a->OperStatus == IfOperStatusUp;
            out.push_back(std::move(itf));
        }
    }
#else
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) detail::throw_os(ErrorKind::Network, "getifaddrs", errno);
    for (ifaddrs* p = list; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;
        int family = p->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        Interface itf;
        itf.name = p->ifa_name ? p->ifa_name : "";
        itf.address = detail::sockaddr_to_string(p->ifa_addr);
        itf.ipv6 = family == AF_INET6;
        itf.loopback = (p->ifa_flags & IFF_LOOPBACK) != 0;
        itf.up = (p->ifa_flags & IFF_UP) != 0;
        out.push_back(std::move(itf));
    }
    ::freeifaddrs(list);
#endif
    return out;
}

bool is_port_open(const std::string& host, std::uint16_t port, Milliseconds timeout) {
    try {
        auto s = tcp::connect(host, port, timeout);
        s->close();
        return true;
    } catch (const Error&) {
        return false;
    }
}

std::vector<std::uint16_t> scan_ports(const std::string& host, const std::vector<std::uint16_t>& ports,
                                      Milliseconds timeout, unsigned concurrency) {
    std::vector<std::uint16_t> open;
    std::mutex mu;
    std::atomic<std::size_t> next{0};
    unsigned workers = std::max(1u, std::min<unsigned>(concurrency, static_cast<unsigned>(ports.size())));
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            for (std::size_t i = next++; i < ports.size(); i = next++) {
                if (is_port_open(host, ports[i], timeout)) {
                    std::lock_guard<std::mutex> lock(mu);
                    open.push_back(ports[i]);
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    std::sort(open.begin(), open.end());
    return open;
}

std::vector<std::uint16_t> port_range(std::uint16_t first, std::uint16_t last) {
    std::vector<std::uint16_t> out;
    if (last < first) return out;
    out.reserve(static_cast<std::size_t>(last - first) + 1);
    for (std::uint32_t p = first; p <= last; ++p) out.push_back(static_cast<std::uint16_t>(p));
    return out;
}

std::optional<double> tcp_ping(const std::string& host, std::uint16_t port, Milliseconds timeout) {
    // Resolve first so DNS time is not counted as latency.
    std::string ip;
    try {
        detail::AddrInfo holder;
        addrinfo* p = detail::resolve(host, port, AF_UNSPEC, SOCK_STREAM, 0, holder);
        ip = detail::sockaddr_to_string(p->ai_addr);
    } catch (const Error&) {
        return std::nullopt;
    }
    auto start = std::chrono::steady_clock::now();
    try {
        auto s = tcp::connect(ip, port, timeout);
        auto end = std::chrono::steady_clock::now();
        s->close();
        return std::chrono::duration<double, std::milli>(end - start).count();
    } catch (const Error&) {
        return std::nullopt;
    }
}

bool wait_for_port(const std::string& host, std::uint16_t port, Milliseconds total,
                   Milliseconds interval) {
    auto deadline = std::chrono::steady_clock::now() + total;
    for (;;) {
        if (is_port_open(host, port, std::min(interval * 5, Milliseconds(1000)))) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(interval);
    }
}

std::string public_ip(Milliseconds timeout) {
    http::Options opts;
    opts.timeout = timeout;
    auto res = http::get("https://api.ipify.org", {}, opts);
    res.raise_for_status();
    std::string ip = res.body;
    while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' ')) ip.pop_back();
    return ip;
}

bool is_ipv4(std::string_view text) {
    detail::net_init();
    std::string s(text);
    in_addr addr{};
    return ::inet_pton(AF_INET, s.c_str(), &addr) == 1;
}

bool is_ipv6(std::string_view text) {
    detail::net_init();
    std::string s(text);
    in6_addr addr{};
    return ::inet_pton(AF_INET6, s.c_str(), &addr) == 1;
}

std::string version() {
    detail::ensure_curl();
    return std::string("netc++ 1.0.0 (") + curl_version() + ")";
}

}  // namespace netcpp::util
