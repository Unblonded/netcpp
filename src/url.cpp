#include "netcpp/url.hpp"

#include "netcpp/encoding.hpp"
#include "netcpp/error.hpp"

#include <algorithm>
#include <cctype>

namespace netcpp {

std::string build_query(const Params& params) {
    std::string out;
    for (const auto& [key, value] : params) {
        if (!out.empty()) out.push_back('&');
        out += url_encode(key);
        out.push_back('=');
        out += url_encode(value);
    }
    return out;
}

Params parse_query(std::string_view query) {
    Params out;
    if (!query.empty() && query.front() == '?') query.remove_prefix(1);
    while (!query.empty()) {
        std::size_t amp = query.find('&');
        std::string_view pair = query.substr(0, amp);
        if (!pair.empty()) {
            std::size_t eq = pair.find('=');
            if (eq == std::string_view::npos) {
                out.emplace_back(url_decode(pair), "");
            } else {
                out.emplace_back(url_decode(pair.substr(0, eq)), url_decode(pair.substr(eq + 1)));
            }
        }
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return out;
}

std::string join_url(std::string_view base, std::string_view path) {
    if (path.empty()) return std::string(base);
    if (base.empty()) return std::string(path);
    if (path.front() == '?' || path.front() == '#') return std::string(base) + std::string(path);
    bool base_slash = base.back() == '/';
    bool path_slash = path.front() == '/';
    if (base_slash && path_slash) return std::string(base) + std::string(path.substr(1));
    if (!base_slash && !path_slash) return std::string(base) + "/" + std::string(path);
    return std::string(base) + std::string(path);
}

std::uint16_t default_port(std::string_view scheme) {
    if (scheme == "http" || scheme == "ws") return 80;
    if (scheme == "https" || scheme == "wss") return 443;
    if (scheme == "ftp") return 21;
    if (scheme == "ftps") return 990;
    if (scheme == "sftp" || scheme == "scp" || scheme == "ssh") return 22;
    if (scheme == "smtp") return 25;
    if (scheme == "smtps") return 465;
    if (scheme == "imap") return 143;
    if (scheme == "imaps") return 993;
    if (scheme == "pop3") return 110;
    if (scheme == "pop3s") return 995;
    if (scheme == "mqtt") return 1883;
    if (scheme == "ldap") return 389;
    return 0;
}

Url Url::parse(std::string_view s) {
    Url u;
    std::size_t sep = s.find("://");
    if (sep == std::string_view::npos || sep == 0) {
        throw Error(ErrorKind::InvalidArgument, "URL has no scheme: " + std::string(s));
    }
    u.scheme.assign(s.substr(0, sep));
    std::transform(u.scheme.begin(), u.scheme.end(), u.scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    s.remove_prefix(sep + 3);

    std::size_t end = s.find_first_of("/?#");
    std::string_view authority = s.substr(0, end);
    std::string_view rest = end == std::string_view::npos ? std::string_view{} : s.substr(end);

    std::size_t at = authority.rfind('@');
    if (at != std::string_view::npos) {
        std::string_view info = authority.substr(0, at);
        std::size_t colon = info.find(':');
        u.user = url_decode(info.substr(0, colon), false);
        if (colon != std::string_view::npos) u.password = url_decode(info.substr(colon + 1), false);
        authority.remove_prefix(at + 1);
    }

    std::string_view port_text;
    if (!authority.empty() && authority.front() == '[') {
        std::size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            throw Error(ErrorKind::InvalidArgument, "unterminated IPv6 host in URL");
        }
        u.host.assign(authority.substr(1, close - 1));
        std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') throw Error(ErrorKind::InvalidArgument, "bad URL authority");
            port_text = after.substr(1);
        }
    } else {
        std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            u.host.assign(authority.substr(0, colon));
            port_text = authority.substr(colon + 1);
        } else {
            u.host.assign(authority);
        }
    }
    if (!port_text.empty()) {
        unsigned long value = 0;
        for (char c : port_text) {
            if (c < '0' || c > '9') throw Error(ErrorKind::InvalidArgument, "bad port in URL");
            value = value * 10 + static_cast<unsigned long>(c - '0');
            if (value > 65535) throw Error(ErrorKind::InvalidArgument, "port out of range in URL");
        }
        u.port = static_cast<std::uint16_t>(value);
    }
    if (u.host.empty() && u.scheme != "file") {
        throw Error(ErrorKind::InvalidArgument, "URL has no host");
    }

    std::size_t hash = rest.find('#');
    if (hash != std::string_view::npos) {
        u.fragment.assign(rest.substr(hash + 1));
        rest = rest.substr(0, hash);
    }
    std::size_t q = rest.find('?');
    if (q != std::string_view::npos) {
        u.query.assign(rest.substr(q + 1));
        rest = rest.substr(0, q);
    }
    u.path.assign(rest);
    return u;
}

std::uint16_t Url::effective_port() const { return port != 0 ? port : default_port(scheme); }

std::string Url::host_port() const {
    std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    if (port != 0) h += ":" + std::to_string(port);
    return h;
}

std::string Url::target() const {
    std::string t = path.empty() ? "/" : path;
    if (!query.empty()) t += "?" + query;
    return t;
}

std::string Url::str() const {
    std::string out = scheme + "://";
    if (!user.empty()) {
        out += url_encode(user);
        if (!password.empty()) out += ":" + url_encode(password);
        out += "@";
    }
    out += host_port();
    out += path;
    if (!query.empty()) out += "?" + query;
    if (!fragment.empty()) out += "#" + fragment;
    return out;
}

}  // namespace netcpp
