#include <netcpp/netcpp.hpp>

#include <iostream>

using namespace std::chrono_literals;

int main() {
    try {
        auto api = netcpp::api("https://httpbin.org");
        api->header("Accept", "application/json")->timeout(10s);

        auto res = api->get("/get", {{"hello", "world"}});
        std::cout << "GET status: " << res.status << "\n" << res.body << "\n";

        auto posted = api->post("/post", netcpp::Body::json(R"({"name":"netc++"})"));
        std::cout << "POST status: " << posted.status << "\n";
    } catch (const netcpp::Error& e) {
        std::cerr << "Network error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
