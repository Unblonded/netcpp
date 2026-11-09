#pragma once

// netc++ — cross-platform network utility library.
//
//   auto api = netcpp::api("https://api.example.com");
//   auto res = api->bearer(token)->post("/users", netcpp::Body::json(R"({"name":"bob"})"));
//
// Include this single header to get everything.

#define NETCPP_VERSION_MAJOR 1
#define NETCPP_VERSION_MINOR 0
#define NETCPP_VERSION_PATCH 0
#define NETCPP_VERSION "1.0.0"

#include "netcpp/dns.hpp"
#include "netcpp/encoding.hpp"
#include "netcpp/error.hpp"
#include "netcpp/http.hpp"
#include "netcpp/socket.hpp"
#include "netcpp/url.hpp"
#include "netcpp/utils.hpp"
#include "netcpp/websocket.hpp"

namespace netcpp {

/// Entry point: an HTTP client used through the arrow operator.
inline http::Client::Ptr api(std::string base_url = "") {
    return http::Client::create(std::move(base_url));
}

using http::Body;
using http::Headers;
using http::Multipart;
using http::Request;
using http::Response;

}  // namespace netcpp
