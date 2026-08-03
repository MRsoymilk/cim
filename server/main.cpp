#include "App.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <unordered_map>
#include <string_view>

using json = nlohmann::json;

int main() {
    std::unordered_map<std::string, std::string> online_users;

    uWS::App().get("/users", [&online_users](auto* res, auto* req) {
        json users_arr = json::array();
        for (const auto& [uname, _] : online_users) {
            users_arr.push_back(uname);
        }
        res->writeHeader("Content-Type", "application/json");
        res->end(users_arr.dump());
    }).post("/join", [&online_users](auto* res, auto* req) {
        std::string* buffer = new std::string();
        res->onData([res, buffer, &online_users](std::string_view chunk, bool last) {
            buffer->append(chunk.data(), chunk.length());
            if (last) {
                try {
                    auto j = json::parse(*buffer);
                    std::string username = j.value("username", "");
                    if (!username.empty()) {
                        online_users[username] = "active";
                        std::cout << "[Server] >>> User Joined: " << username << " | Total Online Users: " << online_users.size() << std::endl;
                    }
                } catch (...) {}
                delete buffer;
                res->writeHeader("Content-Type", "application/json");
                res->end("{\"status\":\"ok\"}");
            }
        });
        res->onAborted([buffer]() {
            delete buffer;
        });
    }).post("/leave", [&online_users](auto* res, auto* req) {
        std::string* buffer = new std::string();
        res->onData([res, buffer, &online_users](std::string_view chunk, bool last) {
            buffer->append(chunk.data(), chunk.length());
            if (last) {
                try {
                    auto j = json::parse(*buffer);
                    std::string username = j.value("username", "");
                    if (!username.empty() && online_users.count(username)) {
                        online_users.erase(username);
                        std::cout << "[Server] <<< User Left: " << username << " | Total Online Users: " << online_users.size() << std::endl;
                    }
                } catch (...) {}
                delete buffer;
                res->writeHeader("Content-Type", "application/json");
                res->end("{\"status\":\"ok\"}");
            }
        });
        res->onAborted([buffer]() {
            delete buffer;
        });
    }).listen(9001, [](auto* listen_socket) {
        if (listen_socket) {
            std::cout << "[cim-server] Relay server listening on port 9001..." << std::endl;
        } else {
            std::cerr << "[cim-server] Failed to listen on port 9001!" << std::endl;
        }
    }).run();

    return 0;
}
