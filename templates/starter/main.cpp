#include <netcpp/netcpp.hpp>

#include <iostream>

using namespace std::chrono_literals;

int main() {
    try {
        auto api = netcpp::api("https://httpbin.org");
        api->header("Accept", "application/json")->timeout(10s);

        auto res = api->get("/get", {{"hello", "netc++"}});
        std::cout << "GET /get -> " << res.status << "\n" << res.body << "\n";

        auto posted = api->post("/post", netcpp::Body::json(R"({"it":"works"})"));
        std::cout << "POST /post -> " << posted.status << "\n";
    } catch (const netcpp::Error& e) {
        std::cerr << "network error: " << e.what() << "\n";
        return 1;
    }
}
