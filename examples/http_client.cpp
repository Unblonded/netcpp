// HTTP client tour: everything goes through the arrow operator.
#include <netcpp/netcpp.hpp>

#include <iostream>

using namespace std::chrono_literals;

int main() {
    try {
        auto api = netcpp::api("https://httpbin.org");

        // Configure once, chain with ->
        api->header("Accept", "application/json")
           ->user_agent("netc++-example/1.0")
           ->timeout(15s)
           ->retries(2);

        // GET with query parameters
        auto res = api->get("/get", {{"search", "netc++"}, {"page", "1"}});
        std::cout << "GET  " << res.status << " in " << res.elapsed.count() << " ms\n";

        // POST JSON
        auto created = api->post("/post", netcpp::Body::json(R"({"name":"bob","admin":false})"));
        std::cout << "POST " << created.status << " content-type=" << created.header("Content-Type") << "\n";

        // Form body via initializer list, auth chained inline
        auto form = api->bearer("my-token")->put("/put", {{"field", "value"}});
        std::cout << "PUT  " << form.status << "\n";

        // PATCH / DELETE / HEAD
        std::cout << "PATCH  " << api->patch("/patch", "raw text").status << "\n";
        std::cout << "DELETE " << api->del("/delete").status << "\n";
        std::cout << "HEAD   " << api->head("/get").status << "\n";

        // Async requests in parallel
        auto a = api->get_async("/delay/1");
        auto b = api->get_async("/uuid");
        std::cout << "async: " << a.get().status << " / " << b.get().body;

        // Download with progress
        api->download("/bytes/102400", "download.bin",
                      [](std::uint64_t now, std::uint64_t total, std::uint64_t, std::uint64_t) {
                          if (total) std::cout << "\rdownloading " << (now * 100 / total) << "%" << std::flush;
                          return true;  // false cancels
                      });
        std::cout << "\nsaved download.bin\n";

        // Error handling
        auto missing = api->get("/status/404");
        if (!missing) std::cout << "404 handled: ok() == false\n";
        missing.raise_for_status();  // throws netcpp::Error(ErrorKind::Http)
    } catch (const netcpp::Error& e) {
        std::cerr << "netcpp error [" << netcpp::to_string(e.kind()) << "]: " << e.what() << "\n";
    }
}
