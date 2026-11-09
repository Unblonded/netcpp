// Prints local network information.
//   netinfo [host-to-resolve]
#include <netcpp/netcpp.hpp>

#include <iostream>

int main(int argc, char** argv) {
    std::cout << netcpp::util::version() << "\n\n";
    std::cout << "hostname: " << netcpp::util::hostname() << "\n\ninterfaces:\n";
    for (const auto& itf : netcpp::util::interfaces()) {
        std::cout << "  " << itf.name << "  " << itf.address << (itf.ipv6 ? "  (v6)" : "")
                  << (itf.loopback ? "  loopback" : "") << (itf.up ? "" : "  down") << "\n";
    }

    std::string host = argc > 1 ? argv[1] : "example.com";
    try {
        std::cout << "\n" << host << ":\n";
        for (const auto& ip : netcpp::dns::resolve(host)) std::cout << "  " << ip << "\n";
    } catch (const netcpp::Error& e) {
        std::cout << "  dns error: " << e.what() << "\n";
    }

    try {
        std::cout << "\npublic ip: " << netcpp::util::public_ip() << "\n";
    } catch (const netcpp::Error& e) {
        std::cout << "\npublic ip unavailable: " << e.what() << "\n";
    }
}
