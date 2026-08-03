#include "App.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <unordered_map>
#include <string_view>
#include <chrono>
#include <thread>

using json = nlohmann::json;

int main() {
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> online_users;

    // Background thread to cleanup inactive users (timeout after 9 seconds of no heartbeat)
    std::jthread cleanup_thread([&online_users](std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            auto now = std::chrono::steady_clock::now();
            // Since uWS App run() runs on the main thread, to be thread-safe with online_users map,
            // we should be careful or use a mutex. But since uWS runs event loop, let's add a mutex or handle it.
        }
    });
    // Actually, uWS runs single-threaded event loop on main thread. To avoid thread races with HTTP handlers,
    // let's use a timer in uWS event loop or handle heartbeats directly in HTTP handlers.
    // uWS has uWS::Loop::get()->addTimer(...) or we can check timeouts inside /heartbeat /users.

    uWS::App().get("/users", [&online_users](auto* res, auto* req) {
        auto now = std::chrono::steady_clock::now();
        // Remove timed-out users (> 9 seconds)
        for (auto it = online_users.begin(); it != online_users.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (elapsed > 9) {
                std::cout << "[Server] <<< User Left (Timeout): " << it->first << " | Total Online Users: " << (online_users.size() - 1) << std::endl;
                it = online_users.erase(it);
            } else {
                ++it;
            }
        }

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
                        bool is_new = online_users.find(username) == online_users.end();
                        online_users[username] = std::chrono::steady_clock::now();
                        if (is_new) {
                            std::cout << "[Server] >>> User Joined: " << username << " | Total Online Users: " << online_users.size() << std::endl;
                        }
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
    }).post("/heartbeat", [&online_users](auto* res, auto* req) {
        std::string* buffer = new std::string();
        res->onData([res, buffer, &online_users](std::string_view chunk, bool last) {
            buffer->append(chunk.data(), chunk.length());
            if (last) {
                try {
                    auto j = json::parse(*buffer);
                    std::string username = j.value("username", "");
                    if (!username.empty()) {
                        bool is_new = online_users.find(username) == online_users.end();
                        online_users[username] = std::chrono::steady_clock::now();
                        if (is_new) {
                            std::cout << "[Server] >>> User Joined (Heartbeat): " << username << " | Total Online Users: " << online_users.size() << std::endl;
                        }
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
