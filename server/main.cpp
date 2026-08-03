#include "App.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <unordered_map>
#include <string_view>

using json = nlohmann::json;

struct PerSocketData {
    std::string username;
};

int main() {
    std::unordered_map<std::string, uWS::WebSocket<false, true, PerSocketData>*> clients;

    uWS::App().ws<PerSocketData>("/*", {
        .compression = uWS::SHARED_COMPRESSOR,
        .maxPayloadLength = 16 * 1024 * 1024,
        .idleTimeout = 120,
        .maxBackpressure = 16 * 1024 * 1024,
        .closeOnBackpressureLimit = false,
        .resetIdleTimeoutOnSend = false,
        .sendPingsAutomatically = true,

        .open = [](auto* /*ws*/) {
            std::cout << "[Server] Client connected." << std::endl;
        },

        .message = [&clients](auto* ws, std::string_view message, uWS::OpCode opCode) {
            auto* userData = ws->getUserData();
            try {
                auto j = json::parse(message);
                std::string type = j.value("type", "");

                if (type == "register") {
                    std::string username = j.value("username", "");
                    if (!username.empty()) {
                        userData->username = username;
                        clients[username] = ws;
                        std::cout << "[Server] Registered user: " << username << std::endl;
                        json res;
                        res["type"] = "system";
                        res["message"] = "Registered successfully";
                        ws->send(res.dump(), opCode, false);
                    }
                } else if (type == "chat") {
                    std::string to = j.value("to", "");
                    std::string content = j.value("content", "");
                    std::string from = userData->username;

                    std::cout << "[Server] Chat from " << from << " to " << to << ": " << content << std::endl;

                    auto it = clients.find(to);
                    if (it != clients.end()) {
                        json outgoing;
                        outgoing["type"] = "chat";
                        outgoing["from"] = from;
                        outgoing["content"] = content;
                        it->second->send(outgoing.dump(), opCode, false);
                    } else {
                        json err;
                        err["type"] = "system";
                        err["message"] = "User " + to + " is offline.";
                        ws->send(err.dump(), opCode, false);
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[Server] JSON parse error: " << e.what() << std::endl;
            }
        },

        .close = [&clients](auto* ws, int /*code*/, std::string_view /*message*/) {
            auto* userData = ws->getUserData();
            if (!userData->username.empty()) {
                clients.erase(userData->username);
                std::cout << "[Server] User disconnected: " << userData->username << std::endl;
            } else {
                std::cout << "[Server] Unregistered client disconnected." << std::endl;
            }
        }
    }).listen(9001, [](auto* listen_socket) {
        if (listen_socket) {
            std::cout << "[cim-server] Relay server listening on port 9001..." << std::endl;
        } else {
            std::cerr << "[cim-server] Failed to listen on port 9001!" << std::endl;
        }
    }).run();

    return 0;
}
