#include "netcpp/encoding.hpp"

#include "netcpp/error.hpp"

#include <cstdint>

namespace netcpp {

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::uint32_t rotl(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
}  // namespace

std::string base64_encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        std::uint32_t n = (std::uint32_t(std::uint8_t(data[i])) << 16) |
                          (std::uint32_t(std::uint8_t(data[i + 1])) << 8) |
                          std::uint32_t(std::uint8_t(data[i + 2]));
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(kB64[(n >> 6) & 63]);
        out.push_back(kB64[n & 63]);
        i += 3;
    }
    std::size_t rest = data.size() - i;
    if (rest == 1) {
        std::uint32_t n = std::uint32_t(std::uint8_t(data[i])) << 16;
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out += "==";
    } else if (rest == 2) {
        std::uint32_t n = (std::uint32_t(std::uint8_t(data[i])) << 16) |
                          (std::uint32_t(std::uint8_t(data[i + 1])) << 8);
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(kB64[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::string base64_decode(std::string_view data) {
    std::string out;
    out.reserve(data.size() / 4 * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : data) {
        if (c == '=' ) break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int v = b64_value(c);
        if (v < 0) throw Error(ErrorKind::InvalidArgument, "invalid base64 input");
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string url_encode(std::string_view text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string url_decode(std::string_view text, bool plus_as_space) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '%' && i + 2 < text.size()) {
            int hi = hex_value(text[i + 1]);
            int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c == '+' && plus_as_space ? ' ' : c);
    }
    return out;
}

std::string sha1(std::string_view data) {
    std::uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476,
                  h4 = 0xC3D2E1F0;
    std::string msg(data);
    std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFF));

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(std::uint8_t(msg[off + 4 * i])) << 24) |
                   (std::uint32_t(std::uint8_t(msg[off + 4 * i + 1])) << 16) |
                   (std::uint32_t(std::uint8_t(msg[off + 4 * i + 2])) << 8) |
                   std::uint32_t(std::uint8_t(msg[off + 4 * i + 3]));
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::string out;
    out.reserve(20);
    for (std::uint32_t h : {h0, h1, h2, h3, h4}) {
        out.push_back(static_cast<char>((h >> 24) & 0xFF));
        out.push_back(static_cast<char>((h >> 16) & 0xFF));
        out.push_back(static_cast<char>((h >> 8) & 0xFF));
        out.push_back(static_cast<char>(h & 0xFF));
    }
    return out;
}

std::string hex_encode(std::string_view data) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char c : data) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
    return out;
}

}  // namespace netcpp
