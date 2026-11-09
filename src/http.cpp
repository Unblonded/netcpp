#include "netcpp/http.hpp"

#include "internal.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace netcpp::http {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------
Headers::Headers(std::initializer_list<Item> init) {
    for (const auto& [k, v] : init) add(k, v);
}

Headers& Headers::set(const std::string& name, const std::string& value) {
    remove(name);
    items_.emplace_back(name, value);
    return *this;
}

Headers& Headers::add(const std::string& name, const std::string& value) {
    items_.emplace_back(name, value);
    return *this;
}

Headers& Headers::remove(const std::string& name) {
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const Item& it) { return iequals(it.first, name); }),
                 items_.end());
    return *this;
}

Headers& Headers::merge(const Headers& other) {
    for (const auto& [k, v] : other) set(k, v);
    return *this;
}

std::string Headers::get(const std::string& name, const std::string& fallback) const {
    for (const auto& [k, v] : items_)
        if (iequals(k, name)) return v;
    return fallback;
}

std::vector<std::string> Headers::get_all(const std::string& name) const {
    std::vector<std::string> out;
    for (const auto& [k, v] : items_)
        if (iequals(k, name)) out.push_back(v);
    return out;
}

bool Headers::has(const std::string& name) const {
    return std::any_of(items_.begin(), items_.end(),
                       [&](const Item& it) { return iequals(it.first, name); });
}

// ---------------------------------------------------------------------------
// Multipart / Body / Response
// ---------------------------------------------------------------------------
Multipart& Multipart::field(const std::string& name, const std::string& value) {
    parts_.push_back(Part{name, value, "", "", ""});
    return *this;
}

Multipart& Multipart::file(const std::string& name, const std::string& path,
                           const std::string& content_type) {
    parts_.push_back(Part{name, "", path, "", content_type});
    return *this;
}

Multipart& Multipart::data(const std::string& name, const std::string& bytes,
                           const std::string& filename, const std::string& content_type) {
    parts_.push_back(Part{name, bytes, "", filename, content_type});
    return *this;
}

Body::Body(std::string data, std::string content_type)
    : data_(std::move(data)), content_type_(std::move(content_type)) {}
Body::Body(const char* data) : data_(data ? data : "") {}
Body::Body(std::initializer_list<std::pair<std::string, std::string>> form)
    : data_(build_query(Params(form.begin(), form.end()))),
      content_type_("application/x-www-form-urlencoded") {}
Body::Body(Multipart multipart) : multipart_(std::move(multipart)) {}

Body Body::json(std::string json) { return Body(std::move(json), "application/json"); }
Body Body::text(std::string text) { return Body(std::move(text), "text/plain; charset=utf-8"); }
Body Body::form(const Params& fields) {
    return Body(build_query(fields), "application/x-www-form-urlencoded");
}
Body Body::bytes(std::string data, std::string content_type) {
    return Body(std::move(data), std::move(content_type));
}
Body Body::file(std::string path, std::string content_type) {
    Body b;
    b.file_path_ = std::move(path);
    b.content_type_ = std::move(content_type);
    return b;
}
Body Body::multipart(Multipart multipart) { return Body(std::move(multipart)); }

bool Body::empty() const { return data_.empty() && file_path_.empty() && !multipart_; }

const Response& Response::raise_for_status() const {
    if (status >= 400) {
        throw Error(ErrorKind::Http, "HTTP " + std::to_string(status) + " for " + url,
                    static_cast<int>(status));
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Client state
// ---------------------------------------------------------------------------
namespace {

struct Config {
    std::string base_url;
    Headers headers;
    std::string user_agent = "netc++/1.0";
    Milliseconds timeout{0};
    Milliseconds connect_timeout{10000};
    bool follow = true;
    long max_redirects = 10;
    bool verify = true;
    std::string ca_bundle;
    std::string cert, key, key_password;
    std::string proxy;
    bool basic = false;
    std::string user, password;
    int retries = 0;
    Milliseconds backoff{200};
    bool cookies = true;
    bool http2 = false;
    bool verbose = false;
};

}  // namespace

struct Client::State {
    mutable std::mutex mu;
    Config cfg;
    CURLSH* share = nullptr;
    std::array<std::mutex, CURL_LOCK_DATA_LAST> locks;

    Config snapshot() const {
        std::lock_guard<std::mutex> lock(mu);
        return cfg;
    }
};

namespace {

void share_lock(CURL*, curl_lock_data data, curl_lock_access, void* userptr) {
    static_cast<std::array<std::mutex, CURL_LOCK_DATA_LAST>*>(userptr)->at(data).lock();
}

void share_unlock(CURL*, curl_lock_data data, void* userptr) {
    static_cast<std::array<std::mutex, CURL_LOCK_DATA_LAST>*>(userptr)->at(data).unlock();
}

struct Transfer {
    std::string body;
    Headers headers;
    const DataFn* on_data = nullptr;
    const ProgressFn* on_progress = nullptr;
    std::exception_ptr callback_error;
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* t = static_cast<Transfer*>(userdata);
    size_t n = size * nmemb;
    if (t->on_data && *t->on_data) {
        try {
            if (!(*t->on_data)(ptr, n)) return 0;
        } catch (...) {
            t->callback_error = std::current_exception();
            return 0;
        }
    } else {
        t->body.append(ptr, n);
    }
    return n;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* t = static_cast<Transfer*>(userdata);
    size_t n = size * nitems;
    std::string_view line = trim(std::string_view(buffer, n));
    if (line.rfind("HTTP/", 0) == 0) {
        t->headers.clear();  // new response (redirect / 100-continue)
    } else {
        std::size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            t->headers.add(std::string(trim(line.substr(0, colon))),
                           std::string(trim(line.substr(colon + 1))));
        }
    }
    return n;
}

int progress_cb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                curl_off_t ulnow) {
    auto* t = static_cast<Transfer*>(userdata);
    try {
        bool keep = (*t->on_progress)(static_cast<std::uint64_t>(dlnow),
                                      static_cast<std::uint64_t>(dltotal),
                                      static_cast<std::uint64_t>(ulnow),
                                      static_cast<std::uint64_t>(ultotal));
        return keep ? 0 : 1;
    } catch (...) {
        t->callback_error = std::current_exception();
        return 1;
    }
}

size_t read_file_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    return std::fread(buffer, 1, size * nitems, static_cast<FILE*>(userdata));
}

Error curl_error(CURLcode rc, const char* errbuf, const std::string& url) {
    std::string msg = (errbuf && errbuf[0]) ? errbuf : curl_easy_strerror(rc);
    ErrorKind kind = ErrorKind::Network;
    switch (rc) {
        case CURLE_OPERATION_TIMEDOUT: kind = ErrorKind::Timeout; break;
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY: kind = ErrorKind::Dns; break;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE: kind = ErrorKind::Tls; break;
        case CURLE_ABORTED_BY_CALLBACK:
        case CURLE_WRITE_ERROR: kind = ErrorKind::Aborted; break;
        case CURLE_URL_MALFORMAT:
        case CURLE_UNSUPPORTED_PROTOCOL: kind = ErrorKind::InvalidArgument; break;
        case CURLE_READ_ERROR:
        case CURLE_FILE_COULDNT_READ_FILE: kind = ErrorKind::Io; break;
        default: break;
    }
    return Error(kind, msg + " (" + url + ")", static_cast<int>(rc));
}

struct SlistDeleter {
    void operator()(curl_slist* l) const { curl_slist_free_all(l); }
};
struct MimeDeleter {
    void operator()(curl_mime* m) const { curl_mime_free(m); }
};
struct FileDeleter {
    void operator()(FILE* f) const { std::fclose(f); }
};
struct EasyDeleter {
    void operator()(CURL* h) const { curl_easy_cleanup(h); }
};

bool retryable_status(long status) {
    return status == 429 || status == 502 || status == 503 || status == 504;
}

}  // namespace

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
Client::Ptr Client::create(std::string base_url) {
    netcpp::detail::ensure_curl();
    return std::make_shared<Client>(Private{}, std::move(base_url));
}

Client::Client(Private, std::string base_url) : state_(std::make_unique<State>()) {
    state_->cfg.base_url = std::move(base_url);
    state_->share = curl_share_init();
    if (state_->share) {
        curl_share_setopt(state_->share, CURLSHOPT_LOCKFUNC, share_lock);
        curl_share_setopt(state_->share, CURLSHOPT_UNLOCKFUNC, share_unlock);
        curl_share_setopt(state_->share, CURLSHOPT_USERDATA, &state_->locks);
        curl_share_setopt(state_->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
        curl_share_setopt(state_->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(state_->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
#if LIBCURL_VERSION_NUM >= 0x073900
        curl_share_setopt(state_->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
#endif
    }
}

Client::~Client() {
    if (state_ && state_->share) curl_share_cleanup(state_->share);
}

#define NETCPP_SETTER(body)                           \
    {                                                 \
        std::lock_guard<std::mutex> lock(state_->mu); \
        Config& c = state_->cfg;                      \
        body;                                         \
    }                                                 \
    return shared_from_this();

Client::Ptr Client::base_url(std::string url) { NETCPP_SETTER(c.base_url = std::move(url)) }
std::string Client::base_url() const { return state_->snapshot().base_url; }
Client::Ptr Client::header(const std::string& name, const std::string& value) {
    NETCPP_SETTER(c.headers.set(name, value))
}
Client::Ptr Client::headers(const Headers& headers) { NETCPP_SETTER(c.headers.merge(headers)) }
Client::Ptr Client::remove_header(const std::string& name) { NETCPP_SETTER(c.headers.remove(name)) }
Client::Ptr Client::bearer(const std::string& token) {
    NETCPP_SETTER(c.headers.set("Authorization", "Bearer " + token))
}
Client::Ptr Client::basic_auth(const std::string& user, const std::string& password) {
    NETCPP_SETTER(c.basic = true; c.user = user; c.password = password)
}
Client::Ptr Client::user_agent(const std::string& agent) { NETCPP_SETTER(c.user_agent = agent) }
Client::Ptr Client::timeout(Milliseconds total) { NETCPP_SETTER(c.timeout = total) }
Client::Ptr Client::connect_timeout(Milliseconds timeout) { NETCPP_SETTER(c.connect_timeout = timeout) }
Client::Ptr Client::follow_redirects(bool enabled, long max_redirects) {
    NETCPP_SETTER(c.follow = enabled; c.max_redirects = max_redirects)
}
Client::Ptr Client::verify_tls(bool enabled) { NETCPP_SETTER(c.verify = enabled) }
Client::Ptr Client::ca_bundle(const std::string& path) { NETCPP_SETTER(c.ca_bundle = path) }
Client::Ptr Client::client_certificate(const std::string& cert_path, const std::string& key_path,
                                       const std::string& key_password) {
    NETCPP_SETTER(c.cert = cert_path; c.key = key_path; c.key_password = key_password)
}
Client::Ptr Client::proxy(const std::string& proxy_url) { NETCPP_SETTER(c.proxy = proxy_url) }
Client::Ptr Client::retries(int count, Milliseconds backoff) {
    NETCPP_SETTER(c.retries = std::max(0, count); c.backoff = backoff)
}
Client::Ptr Client::cookies(bool enabled) { NETCPP_SETTER(c.cookies = enabled) }
Client::Ptr Client::http2(bool enabled) { NETCPP_SETTER(c.http2 = enabled) }
Client::Ptr Client::verbose(bool enabled) { NETCPP_SETTER(c.verbose = enabled) }

#undef NETCPP_SETTER

std::string Client::url_for(const std::string& path, const Params& params) const {
    std::string base = state_->snapshot().base_url;
    std::string url = (path.find("://") != std::string::npos || base.empty()) ? path
                                                                              : join_url(base, path);
    if (!params.empty()) {
        std::size_t hash = url.find('#');
        std::string fragment;
        if (hash != std::string::npos) {
            fragment = url.substr(hash);
            url.erase(hash);
        }
        url += (url.find('?') == std::string::npos ? '?' : '&');
        url += build_query(params);
        url += fragment;
    }
    return url;
}

Request Client::make_request(const std::string& method, const std::string& path,
                             const Params& params, const Body& body, const Options& options) const {
    Request r;
    r.method = method;
    r.url = path;
    r.params = params;
    r.headers = options.headers;
    r.body = body;
    r.timeout = options.timeout;
    r.on_progress = options.on_progress;
    r.on_data = options.on_data;
    return r;
}

namespace {

Response perform_once(CURLSH* share, const Config& c, const Request& req, const std::string& url) {
    std::unique_ptr<CURL, EasyDeleter> easy(curl_easy_init());
    if (!easy) throw Error(ErrorKind::Network, "curl_easy_init failed");
    CURL* h = easy.get();
    char errbuf[CURL_ERROR_SIZE] = {0};
    Transfer t;
    t.on_data = &req.on_data;
    t.on_progress = &req.on_progress;

    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    if (share) curl_easy_setopt(h, CURLOPT_SHARE, share);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, c.follow ? 1L : 0L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, c.max_redirects);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, c.verify ? 1L : 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, c.verify ? 2L : 0L);
    if (!c.ca_bundle.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, c.ca_bundle.c_str());
    if (!c.cert.empty()) curl_easy_setopt(h, CURLOPT_SSLCERT, c.cert.c_str());
    if (!c.key.empty()) curl_easy_setopt(h, CURLOPT_SSLKEY, c.key.c_str());
    if (!c.key_password.empty()) curl_easy_setopt(h, CURLOPT_KEYPASSWD, c.key_password.c_str());
    if (!c.proxy.empty()) curl_easy_setopt(h, CURLOPT_PROXY, c.proxy.c_str());
    if (!c.user_agent.empty()) curl_easy_setopt(h, CURLOPT_USERAGENT, c.user_agent.c_str());
    if (c.basic) {
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(h, CURLOPT_USERNAME, c.user.c_str());
        curl_easy_setopt(h, CURLOPT_PASSWORD, c.password.c_str());
    }
    Milliseconds total = req.timeout ? *req.timeout : c.timeout;
    if (total.count() > 0) curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(total.count()));
    if (c.connect_timeout.count() > 0)
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(c.connect_timeout.count()));
    if (c.cookies) curl_easy_setopt(h, CURLOPT_COOKIEFILE, "");
    if (c.http2) curl_easy_setopt(h, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
    if (c.verbose) curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);
    if (req.on_progress) {
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, &t);
    }

    const std::string method = upper(req.method.empty() ? "GET" : req.method);
    const Body& body = req.body;
    Headers headers = c.headers;
    headers.merge(req.headers);

    std::unique_ptr<curl_mime, MimeDeleter> mime;
    std::unique_ptr<FILE, FileDeleter> upload_file;

    if (method == "HEAD") {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else if (body.multipart_form()) {
        mime.reset(curl_mime_init(h));
        for (const auto& part : body.multipart_form()->parts()) {
            curl_mimepart* mp = curl_mime_addpart(mime.get());
            curl_mime_name(mp, part.name.c_str());
            if (!part.file_path.empty()) {
                if (!std::filesystem::is_regular_file(part.file_path)) {
                    throw Error(ErrorKind::Io, "multipart file not found: " + part.file_path);
                }
                curl_mime_filedata(mp, part.file_path.c_str());
            } else {
                curl_mime_data(mp, part.data.data(), part.data.size());
            }
            if (!part.filename.empty()) curl_mime_filename(mp, part.filename.c_str());
            if (!part.content_type.empty()) curl_mime_type(mp, part.content_type.c_str());
        }
        curl_easy_setopt(h, CURLOPT_MIMEPOST, mime.get());
        if (method != "POST") curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    } else if (!body.file_path().empty()) {
        std::error_code ec;
        auto size = std::filesystem::file_size(body.file_path(), ec);
        if (ec) throw Error(ErrorKind::Io, "cannot read upload file: " + body.file_path());
        upload_file.reset(std::fopen(body.file_path().c_str(), "rb"));
        if (!upload_file) throw Error(ErrorKind::Io, "cannot open upload file: " + body.file_path());
        curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(h, CURLOPT_READFUNCTION, read_file_cb);
        curl_easy_setopt(h, CURLOPT_READDATA, upload_file.get());
        curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));
        if (method != "PUT") curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    } else if (!body.data().empty() || method == "POST") {
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.data().size()));
        curl_easy_setopt(h, CURLOPT_COPYPOSTFIELDS, body.data().c_str());
        if (method != "POST") curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    } else if (method == "GET") {
        curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    if (!body.content_type().empty() && !body.multipart_form() && !headers.has("Content-Type")) {
        headers.set("Content-Type", body.content_type());
    }

    curl_slist* raw_list = nullptr;
    for (const auto& [name, value] : headers) {
        std::string line = value.empty() ? name + ";" : name + ": " + value;
        raw_list = curl_slist_append(raw_list, line.c_str());
    }
    if (!headers.has("Expect")) raw_list = curl_slist_append(raw_list, "Expect:");
    std::unique_ptr<curl_slist, SlistDeleter> list(raw_list);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, list.get());

    auto start = std::chrono::steady_clock::now();
    CURLcode rc = curl_easy_perform(h);
    auto elapsed = std::chrono::duration_cast<Milliseconds>(std::chrono::steady_clock::now() - start);

    if (t.callback_error) std::rethrow_exception(t.callback_error);
    if (rc != CURLE_OK) throw curl_error(rc, errbuf, url);

    Response res;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);
    char* effective = nullptr;
    curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &effective);
    res.url = effective ? effective : url;
    res.elapsed = elapsed;
    res.headers = std::move(t.headers);
    res.body = std::move(t.body);
    return res;
}

}  // namespace

Response Client::send(const Request& req) {
    Config c = state_->snapshot();
    const std::string url = url_for(req.url, req.params);
    // Streaming callbacks would see duplicate data on a retry, so never retry those.
    const int attempts = (req.on_data ? 0 : c.retries) + 1;
    for (int i = 0;; ++i) {
        const bool last = i + 1 >= attempts;
        try {
            Response res = perform_once(state_->share, c, req, url);
            if (!last && retryable_status(res.status)) {
                std::this_thread::sleep_for(c.backoff * (1 << std::min(i, 10)));
                continue;
            }
            return res;
        } catch (const Error& e) {
            bool transient = e.kind() == ErrorKind::Network || e.kind() == ErrorKind::Timeout;
            if (last || !transient) throw;
            std::this_thread::sleep_for(c.backoff * (1 << std::min(i, 10)));
        }
    }
}

Response Client::get(const std::string& path, const Params& params, const Options& options) {
    return send(make_request("GET", path, params, {}, options));
}
Response Client::post(const std::string& path, const Body& body, const Options& options) {
    return send(make_request("POST", path, {}, body, options));
}
Response Client::put(const std::string& path, const Body& body, const Options& options) {
    return send(make_request("PUT", path, {}, body, options));
}
Response Client::patch(const std::string& path, const Body& body, const Options& options) {
    return send(make_request("PATCH", path, {}, body, options));
}
Response Client::del(const std::string& path, const Body& body, const Options& options) {
    return send(make_request("DELETE", path, {}, body, options));
}
Response Client::head(const std::string& path, const Params& params, const Options& options) {
    return send(make_request("HEAD", path, params, {}, options));
}
Response Client::options(const std::string& path, const Options& options) {
    return send(make_request("OPTIONS", path, {}, {}, options));
}
Response Client::request(const std::string& method, const std::string& path, const Body& body,
                         const Options& options) {
    return send(make_request(method, path, {}, body, options));
}

std::future<Response> Client::send_async(Request request) {
    auto self = shared_from_this();
    return std::async(std::launch::async,
                      [self, request = std::move(request)] { return self->send(request); });
}

void Client::send_async(Request request, std::function<void(Response)> on_done,
                        std::function<void(std::exception_ptr)> on_error) {
    auto self = shared_from_this();
    std::thread([self, request = std::move(request), on_done = std::move(on_done),
                 on_error = std::move(on_error)] {
        try {
            Response res = self->send(request);
            if (on_done) on_done(std::move(res));
        } catch (...) {
            if (on_error) on_error(std::current_exception());
        }
    }).detach();
}

std::future<Response> Client::get_async(const std::string& path, const Params& params,
                                        const Options& options) {
    return send_async(make_request("GET", path, params, {}, options));
}
std::future<Response> Client::post_async(const std::string& path, const Body& body,
                                         const Options& options) {
    return send_async(make_request("POST", path, {}, body, options));
}
std::future<Response> Client::put_async(const std::string& path, const Body& body,
                                        const Options& options) {
    return send_async(make_request("PUT", path, {}, body, options));
}
std::future<Response> Client::patch_async(const std::string& path, const Body& body,
                                          const Options& options) {
    return send_async(make_request("PATCH", path, {}, body, options));
}
std::future<Response> Client::del_async(const std::string& path, const Body& body,
                                        const Options& options) {
    return send_async(make_request("DELETE", path, {}, body, options));
}

Response Client::download(const std::string& path, const std::string& file_path,
                          ProgressFn on_progress) {
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    if (!out) throw Error(ErrorKind::Io, "cannot open for writing: " + file_path);
    Request r;
    r.method = "GET";
    r.url = path;
    r.on_progress = std::move(on_progress);
    r.on_data = [&out](const char* data, std::size_t size) {
        out.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(out);
    };
    Response res;
    try {
        res = send(r);
    } catch (...) {
        out.close();
        std::remove(file_path.c_str());
        throw;
    }
    out.close();
    if (!res.ok()) std::remove(file_path.c_str());
    return res;
}

Response Client::upload(const std::string& path, const std::string& file_path,
                        const std::string& method, ProgressFn on_progress) {
    Request r;
    r.method = method;
    r.url = path;
    r.body = Body::file(file_path);
    r.on_progress = std::move(on_progress);
    return send(r);
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------
Client::Ptr default_client() {
    static Client::Ptr instance = Client::create();
    return instance;
}

Response get(const std::string& url, const Params& params, const Options& options) {
    return default_client()->get(url, params, options);
}
Response post(const std::string& url, const Body& body, const Options& options) {
    return default_client()->post(url, body, options);
}
Response put(const std::string& url, const Body& body, const Options& options) {
    return default_client()->put(url, body, options);
}
Response patch(const std::string& url, const Body& body, const Options& options) {
    return default_client()->patch(url, body, options);
}
Response del(const std::string& url, const Body& body, const Options& options) {
    return default_client()->del(url, body, options);
}
Response head(const std::string& url, const Params& params, const Options& options) {
    return default_client()->head(url, params, options);
}

}  // namespace netcpp::http
