#pragma once

#include "netcpp/error.hpp"
#include "netcpp/url.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace netcpp::http {

using netcpp::Params;
using Milliseconds = std::chrono::milliseconds;

// ---------------------------------------------------------------------------
// Headers: ordered, case-insensitive multi-map
// ---------------------------------------------------------------------------
class Headers {
public:
    using Item = std::pair<std::string, std::string>;

    Headers() = default;
    Headers(std::initializer_list<Item> init);

    Headers& set(const std::string& name, const std::string& value);  ///< replace
    Headers& add(const std::string& name, const std::string& value);  ///< append
    Headers& remove(const std::string& name);
    /// Copies every header of `other` into this one, replacing same-named ones.
    Headers& merge(const Headers& other);

    std::string get(const std::string& name, const std::string& fallback = "") const;
    std::vector<std::string> get_all(const std::string& name) const;
    bool has(const std::string& name) const;

    void clear() { items_.clear(); }
    std::size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    std::vector<Item>::const_iterator begin() const { return items_.begin(); }
    std::vector<Item>::const_iterator end() const { return items_.end(); }

private:
    std::vector<Item> items_;
};

// ---------------------------------------------------------------------------
// Multipart form (multipart/form-data)
// ---------------------------------------------------------------------------
class Multipart {
public:
    struct Part {
        std::string name;
        std::string data;          ///< in-memory content (unused if file_path set)
        std::string file_path;     ///< stream this file from disk
        std::string filename;      ///< filename reported to the server
        std::string content_type;
    };

    Multipart& field(const std::string& name, const std::string& value);
    Multipart& file(const std::string& name, const std::string& path,
                    const std::string& content_type = "");
    Multipart& data(const std::string& name, const std::string& bytes, const std::string& filename,
                    const std::string& content_type = "application/octet-stream");

    const std::vector<Part>& parts() const { return parts_; }

private:
    std::vector<Part> parts_;
};

// ---------------------------------------------------------------------------
// Body: what a request sends
// ---------------------------------------------------------------------------
class Body {
public:
    Body() = default;
    /// Raw string body, optional content type.
    Body(std::string data, std::string content_type = "");
    Body(const char* data);
    /// `{{"user","bob"},{"age","3"}}` -> application/x-www-form-urlencoded
    Body(std::initializer_list<std::pair<std::string, std::string>> form);
    Body(Multipart multipart);

    static Body json(std::string json);
    static Body text(std::string text);
    static Body form(const Params& fields);
    static Body bytes(std::string data, std::string content_type = "application/octet-stream");
    /// Streams a file from disk as the request body.
    static Body file(std::string path, std::string content_type = "application/octet-stream");
    static Body multipart(Multipart multipart);

    bool empty() const;
    const std::string& data() const { return data_; }
    const std::string& content_type() const { return content_type_; }
    const std::string& file_path() const { return file_path_; }
    const std::optional<Multipart>& multipart_form() const { return multipart_; }

private:
    std::string data_;
    std::string content_type_;
    std::string file_path_;
    std::optional<Multipart> multipart_;
};

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------
struct Response {
    long status = 0;
    Headers headers;
    std::string body;
    std::string url;              ///< final URL after redirects
    Milliseconds elapsed{0};

    bool ok() const { return status >= 200 && status < 300; }
    explicit operator bool() const { return ok(); }
    const std::string& text() const { return body; }
    std::string header(const std::string& name) const { return headers.get(name); }
    /// Throws Error(ErrorKind::Http, ..., status) when status >= 400.
    const Response& raise_for_status() const;
};

/// Progress callback; return false to abort the transfer.
using ProgressFn = std::function<bool(std::uint64_t downloaded, std::uint64_t download_total,
                                      std::uint64_t uploaded, std::uint64_t upload_total)>;
/// Streaming body callback; return false to abort the transfer.
using DataFn = std::function<bool(const char* data, std::size_t size)>;

/// Per-call extras accepted by get/post/... .
struct Options {
    Headers headers;
    std::optional<Milliseconds> timeout;
    ProgressFn on_progress;
    DataFn on_data;  ///< when set, the body is streamed here and Response::body stays empty
};

/// Fully described request, for Client::send().
struct Request {
    std::string method = "GET";
    std::string url;  ///< absolute URL, or a path relative to the client's base URL
    Params params;
    Headers headers;
    Body body;
    std::optional<Milliseconds> timeout;
    ProgressFn on_progress;
    DataFn on_data;
};

// ---------------------------------------------------------------------------
// Client – always used through a shared_ptr:  api->get(...)
// ---------------------------------------------------------------------------
class Client : public std::enable_shared_from_this<Client> {
    struct Private {};

public:
    using Ptr = std::shared_ptr<Client>;

    /// Creates a client. `base_url` is prefixed to relative paths.
    static Ptr create(std::string base_url = "");

    Client(Private, std::string base_url);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // ---- configuration: every setter returns the client so calls chain with -> ----
    Ptr base_url(std::string url);
    std::string base_url() const;
    Ptr header(const std::string& name, const std::string& value);
    Ptr headers(const Headers& headers);
    Ptr remove_header(const std::string& name);
    Ptr bearer(const std::string& token);
    Ptr basic_auth(const std::string& user, const std::string& password);
    Ptr user_agent(const std::string& agent);
    Ptr timeout(Milliseconds total);  ///< 0 = no limit (default)
    Ptr connect_timeout(Milliseconds timeout);
    Ptr follow_redirects(bool enabled = true, long max_redirects = 10);
    Ptr verify_tls(bool enabled = true);
    Ptr ca_bundle(const std::string& path);
    Ptr client_certificate(const std::string& cert_path, const std::string& key_path,
                           const std::string& key_password = "");
    Ptr proxy(const std::string& proxy_url);  ///< http://, https://, socks5://
    Ptr retries(int count, Milliseconds backoff = Milliseconds(200));
    Ptr cookies(bool enabled = true);  ///< in-memory cookie jar shared by this client
    Ptr http2(bool enabled = true);
    Ptr verbose(bool enabled = true);

    // ---- synchronous requests ----
    Response get(const std::string& path, const Params& params = {}, const Options& options = {});
    Response post(const std::string& path, const Body& body = {}, const Options& options = {});
    Response put(const std::string& path, const Body& body = {}, const Options& options = {});
    Response patch(const std::string& path, const Body& body = {}, const Options& options = {});
    Response del(const std::string& path, const Body& body = {}, const Options& options = {});
    Response head(const std::string& path, const Params& params = {}, const Options& options = {});
    Response options(const std::string& path, const Options& options = {});
    /// Any verb, e.g. request("PROPFIND", "/dav").
    Response request(const std::string& method, const std::string& path, const Body& body = {},
                     const Options& options = {});
    Response send(const Request& request);

    // ---- asynchronous requests ----
    std::future<Response> get_async(const std::string& path, const Params& params = {},
                                    const Options& options = {});
    std::future<Response> post_async(const std::string& path, const Body& body = {},
                                     const Options& options = {});
    std::future<Response> put_async(const std::string& path, const Body& body = {},
                                    const Options& options = {});
    std::future<Response> patch_async(const std::string& path, const Body& body = {},
                                      const Options& options = {});
    std::future<Response> del_async(const std::string& path, const Body& body = {},
                                    const Options& options = {});
    std::future<Response> send_async(Request request);
    /// Fire-and-forget with callbacks (runs on a background thread).
    void send_async(Request request, std::function<void(Response)> on_done,
                    std::function<void(std::exception_ptr)> on_error = {});

    // ---- files ----
    /// Saves the response body to `file_path` (deleted again if status is not 2xx).
    Response download(const std::string& path, const std::string& file_path,
                      ProgressFn on_progress = {});
    /// Sends a file as the request body (PUT by default; works for ftp:// too).
    Response upload(const std::string& path, const std::string& file_path,
                    const std::string& method = "PUT", ProgressFn on_progress = {});

    /// Builds the absolute URL used for `path` (+ params).
    std::string url_for(const std::string& path, const Params& params = {}) const;

private:
    struct State;
    std::unique_ptr<State> state_;
    Request make_request(const std::string& method, const std::string& path, const Params& params,
                         const Body& body, const Options& options) const;
};

/// Same as Client::create().
inline Client::Ptr client(std::string base_url = "") { return Client::create(std::move(base_url)); }

/// Shared process-wide client used by the free functions below.
Client::Ptr default_client();

// One-shot helpers: netcpp::http::get("https://example.com")
Response get(const std::string& url, const Params& params = {}, const Options& options = {});
Response post(const std::string& url, const Body& body = {}, const Options& options = {});
Response put(const std::string& url, const Body& body = {}, const Options& options = {});
Response patch(const std::string& url, const Body& body = {}, const Options& options = {});
Response del(const std::string& url, const Body& body = {}, const Options& options = {});
Response head(const std::string& url, const Params& params = {}, const Options& options = {});

}  // namespace netcpp::http
