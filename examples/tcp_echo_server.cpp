// Line-based TCP echo server. Try:  nc 127.0.0.1 7000
//   tcp_echo_server [port]
#include <netcpp/netcpp.hpp>

#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    std::uint16_t port = argc > 1 ? static_cast<std::uint16_t>(std::stoi(argv[1])) : 7000;
    try {
        auto server = netcpp::tcp::listen(port);
        std::cout << "listening on " << server->address() << ":" << server->port() << "\n";
        for (;;) {
            auto client = server->accept();
            std::thread([client] {
                std::string who = client->remote_address() + ":" + std::to_string(client->remote_port());
                std::cout << "+ " << who << "\n";
                try {
                    client->set_timeout(std::chrono::minutes(5));
                    client->send("welcome to netc++ echo\n");
                    while (auto line = client->read_line()) client->send("echo: " + *line + "\n");
                } catch (const netcpp::Error& e) {
                    std::cout << "  " << who << " error: " << e.what() << "\n";
                }
                std::cout << "- " << who << "\n";
            }).detach();
        }
    } catch (const netcpp::Error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
