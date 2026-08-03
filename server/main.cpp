#include "App.h"
#include <iostream>
#include <string>
#include <unordered_map>
#include <string_view>

struct PerSocketData {
    std::string username;
};

std::string extractJsonField(std::string_view json, const std::string& key) {
    std::string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
    if (pos == std::string_view::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string_view::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string_view::npos) return "";
    size_t end_pos = json.find('"', pos + 1);
    if (end_pos == std::string_view::npos) return "";
    return std::string(json.substr(pos + 1, end_pos - pos - 1));
}

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
            std::string type = extractJsonField(message, "type");

            if (type == "register") {
                std::string username = extractJsonField(message, "username");
                if (!username.empty()) {
                    userData->username = username;
                    clients[username] = ws;
                    std::cout << "[Server] Registered user: " << username << std::endl;
                    ws->send("{\"type\":\"system\",\"message\":\"Registered successfully\"}", opCode, false);
                }
            } else if (type == "chat") {
                std::string to = extractJsonField(message, "to");
                std::string content = extractJsonField(message, "content");
                std::string from = userData->username;

                std::cout << "[Server] Chat from " << from << " to " << to << ": " << content << std::endl;

                auto it = clients.find(to);
                if (it != clients.end()) {
                    std::string outgoing = "{\"type\":\"chat\",\"from\":\"" + from + "\",\"content\":\"" + content + "\"}";
                    it->second->send(outgoing, opCode, false);
                } else {
                    std::string err = "{\"type\":\"system\",\"message\":\"User " + to + " is offline.\"}";
                    ws->send(err, opCode, false);
                }
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
