// Concurrent TCP port scanner.
//   port_scanner <host> [first] [last]
// Only scan hosts you own or are authorised to test.
#include <netcpp/netcpp.hpp>

#include <iostream>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: port_scanner <host> [first=1] [last=1024]\n";
        return 2;
    }
    std::string host = argv[1];
    auto first = static_cast<std::uint16_t>(argc > 2 ? std::stoi(argv[2]) : 1);
    auto last = static_cast<std::uint16_t>(argc > 3 ? std::stoi(argv[3]) : 1024);

    try {
        std::cout << "resolving " << host << " -> " << netcpp::dns::resolve_one(host) << "\n";
        auto open = netcpp::util::scan_ports(host, netcpp::util::port_range(first, last), 400ms, 128);
        for (auto p : open) {
            auto rtt = netcpp::util::tcp_ping(host, p);
            std::cout << "  " << p << "/tcp open";
            if (rtt) std::cout << "  (" << *rtt << " ms)";
            std::cout << "\n";
        }
        std::cout << open.size() << " open port(s) in " << first << "-" << last << "\n";
    } catch (const netcpp::Error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
