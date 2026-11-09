// Interactive WebSocket client: type lines, see the echoes.
//   websocket_chat [wss://ws.postman-echo.com/raw]
#include <netcpp/netcpp.hpp>

#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    std::string url = argc > 1 ? argv[1] : "wss://ws.postman-echo.com/raw";
    try {
        auto ws = netcpp::ws::connect(url);
        std::cout << "connected to " << url << " (type /quit to exit)\n";

        std::thread reader([ws] {
            while (auto msg = ws->receive()) {
                std::cout << "< " << (msg->is_text() ? msg->data : "[" + std::to_string(msg->data.size()) + " bytes]")
                          << "\n";
            }
            std::cout << "connection closed (code " << ws->close_code() << ")\n";
        });

        std::string line;
        while (std::getline(std::cin, line) && line != "/quit") ws->send(line);
        ws->close();
        reader.join();
    } catch (const netcpp::Error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
