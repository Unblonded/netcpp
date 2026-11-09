#include "netcpp/dns.hpp"

#include "platform.hpp"

#include <algorithm>

namespace netcpp::dns {

std::vector<std::string> resolve(const std::string& host, Family family) {
    int af = family == Family::IPv4 ? AF_INET : family == Family::IPv6 ? AF_INET6 : AF_UNSPEC;
    detail::AddrInfo holder;
    addrinfo* list = detail::resolve(host, 0, af, SOCK_STREAM, 0, holder);
    std::vector<std::string> out;
    for (addrinfo* p = list; p; p = p->ai_next) {
        std::string ip = detail::sockaddr_to_string(p->ai_addr);
        if (!ip.empty() && std::find(out.begin(), out.end(), ip) == out.end()) out.push_back(ip);
    }
    if (out.empty()) throw Error(ErrorKind::Dns, "no addresses for '" + host + "'");
    return out;
}

std::string resolve_one(const std::string& host, Family family) { return resolve(host, family).front(); }

std::string reverse(const std::string& ip) {
    detail::AddrInfo holder;
    addrinfo* p = detail::resolve(ip, 0, AF_UNSPEC, SOCK_STREAM, AI_NUMERICHOST, holder);
    char name[NI_MAXHOST] = {0};
    int rc = ::getnameinfo(p->ai_addr, static_cast<detail::socklen>(p->ai_addrlen), name,
                           sizeof(name), nullptr, 0, NI_NAMEREQD);
    if (rc != 0) {
#ifdef _WIN32
        std::string reason = detail::error_message(rc);
#else
        std::string reason = ::gai_strerror(rc);
#endif
        throw Error(ErrorKind::Dns, "reverse lookup of " + ip + " failed: " + reason, rc);
    }
    return name;
}

}  // namespace netcpp::dns
