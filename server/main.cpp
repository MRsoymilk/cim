#include "App.h"
#include "Database.hpp"
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <iostream>
#include <string>
#include <unordered_map>
#include <string_view>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

std::string generateToken() {
    unsigned char buf[32];
    randombytes_buf(buf, sizeof(buf));
    std::stringstream ss;
    for (int i = 0; i < 32; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)buf[i];
    }
    return ss.str();
}

std::string getAuthToken(std::string_view auth_header) {
    if (auth_header.starts_with("Bearer ")) {
        return std::string(auth_header.substr(7));
    }
    return "";
}

int main() {
    if (sodium_init() < 0) {
        std::cerr << "[Server] Failed to initialize libsodium!" << std::endl;
        return 1;
    }

    cim::Database db;
    if (!db.init("cim.db")) {
        std::cerr << "[Server] Failed to initialize database!" << std::endl;
        return 1;
    }

    // online_users: username -> last_heartbeat_timestamp
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> online_users;

    uWS::App().post("/auth/register", [&db](auto* res, auto* req) {
        std::string* buffer = new std::string();
        res->onData([res, buffer, &db](std::string_view chunk, bool last) {
            buffer->append(chunk.data(), chunk.length());
            if (last) {
                json response;
                try {
                    auto j = json::parse(*buffer);
                    std::string username = j.value("username", "");
                    std::string password = j.value("password", "");

                    if (username.empty() || password.empty()) {
                        res->writeStatus("400 Bad Request");
                        response["error"] = "Username and password required";
                    } else {
                        char hash[crypto_pwhash_STRBYTES];
                        if (crypto_pwhash_str(hash, password.c_str(), password.size(),
                                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                              crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
                            res->writeStatus("500 Internal Server Error");
                            response["error"] = "Password hashing failed";
                        } else {
                            int64_t user_id = db.registerUser(username, hash);
                            if (user_id < 0) {
                                res->writeStatus("409 Conflict");
                                response["error"] = "Username already exists";
                            } else {
                                std::string token = generateToken();
                                int64_t expires_at = std::time(nullptr) + 86400 * 7; // 7 days
                                db.createSession(token, user_id, expires_at);

                                response["success"] = true;
                                response["user_id"] = user_id;
                                response["username"] = username;
                                response["token"] = token;
                                std::cout << "[Server] Registered & Logged in: " << username << " (ID: " << user_id << ")" << std::endl;
                            }
                        }
                    }
                } catch (...) {
                    res->writeStatus("400 Bad Request");
                    response["error"] = "Invalid JSON";
                }
                delete buffer;
                res->writeHeader("Content-Type", "application/json");
                res->end(response.dump());
            }
        });
        res->onAborted([buffer]() {
            delete buffer;
        });
    }).post("/auth/login", [&db](auto* res, auto* req) {
        std::string* buffer = new std::string();
        res->onData([res, buffer, &db](std::string_view chunk, bool last) {
            buffer->append(chunk.data(), chunk.length());
            if (last) {
                json response;
                try {
                    auto j = json::parse(*buffer);
                    std::string username = j.value("username", "");
                    std::string password = j.value("password", "");

                    int64_t user_id = -1;
                    std::string stored_hash;
                    if (db.getUser(username, user_id, stored_hash) &&
                        crypto_pwhash_str_verify(stored_hash.c_str(), password.c_str(), password.size()) == 0) {
                        
                        std::string token = generateToken();
                        int64_t expires_at = std::time(nullptr) + 86400 * 7;
                        db.createSession(token, user_id, expires_at);

                        response["success"] = true;
                        response["user_id"] = user_id;
                        response["username"] = username;
                        response["token"] = token;
                        std::cout << "[Server] User logged in: " << username << " (ID: " << user_id << ")" << std::endl;
                    } else {
                        res->writeStatus("401 Unauthorized");
                        response["error"] = "Invalid username or password";
                    }
                } catch (...) {
                    res->writeStatus("400 Bad Request");
                    response["error"] = "Invalid JSON";
                }
                delete buffer;
                res->writeHeader("Content-Type", "application/json");
                res->end(response.dump());
            }
        });
        res->onAborted([buffer]() {
            delete buffer;
        });
    }).get("/users", [&db, &online_users](auto* res, auto* req) {
        std::string auth_header(req->getHeader("authorization"));
        std::string token = getAuthToken(auth_header);

        int64_t user_id = -1;
        std::string username;
        if (token.empty() || !db.getUserByToken(token, user_id, username)) {
            res->writeStatus("401 Unauthorized");
            res->end("{\"error\":\"Unauthorized\"}");
            return;
        }

        auto now = std::chrono::steady_clock::now();
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
        auto all_users = db.getAllUsers();
        for (const auto& [u_id, u_name] : all_users) {
            json u;
            u["id"] = u_id;
            u["username"] = u_name;
            u["online"] = online_users.count(u_name) > 0;
            users_arr.push_back(u);
        }

        res->writeHeader("Content-Type", "application/json");
        res->end(users_arr.dump());
    }).post("/join", [&db, &online_users](auto* res, auto* req) {
        std::string auth_header(req->getHeader("authorization"));
        std::string token = getAuthToken(auth_header);

        std::string* buffer = new std::string();
        res->onData([res, buffer, token, &db, &online_users](std::string_view chunk, bool last) {
            buffer->append(chunk.data(), chunk.length());
            if (last) {
                int64_t user_id = -1;
                std::string username;
                if (token.empty() || !db.getUserByToken(token, user_id, username)) {
                    res->writeStatus("401 Unauthorized");
                    res->end("{\"error\":\"Unauthorized\"}");
                    delete buffer;
                    return;
                }

                bool is_new = online_users.find(username) == online_users.end();
                online_users[username] = std::chrono::steady_clock::now();
                if (is_new) {
                    std::cout << "[Server] >>> User Joined: " << username << " (ID: " << user_id << ") | Total Online Users: " << online_users.size() << std::endl;
                }

                delete buffer;
                res->writeHeader("Content-Type", "application/json");
                res->end("{\"status\":\"ok\"}");
            }
        });
        res->onAborted([buffer]() {
            delete buffer;
        });
    }).post("/heartbeat", [&db, &online_users](auto* res, auto* req) {
        std::string auth_header(req->getHeader("authorization"));
        std::string token = getAuthToken(auth_header);

        std::string* buffer = new std::string();
        res->onData([res, buffer, token, &db, &online_users](std::string_view chunk, bool last) {
            buffer->append(chunk.data(), chunk.length());
            if (last) {
                int64_t user_id = -1;
                std::string username;
                if (token.empty() || !db.getUserByToken(token, user_id, username)) {
                    res->writeStatus("401 Unauthorized");
                    res->end("{\"error\":\"Unauthorized\"}");
                    delete buffer;
                    return;
                }

                bool is_new = online_users.find(username) == online_users.end();
                online_users[username] = std::chrono::steady_clock::now();
                if (is_new) {
                    std::cout << "[Server] >>> User Joined (Heartbeat): " << username << " (ID: " << user_id << ") | Total Online Users: " << online_users.size() << std::endl;
                }

                delete buffer;
                res->writeHeader("Content-Type", "application/json");
                res->end("{\"status\":\"ok\"}");
            }
        });
        res->onAborted([buffer]() {
            delete buffer;
        });
    }).post("/leave", [&db, &online_users](auto* res, auto* req) {
        std::string auth_header(req->getHeader("authorization"));
        std::string token = getAuthToken(auth_header);

        std::string* buffer = new std::string();
        res->onData([res, buffer, token, &db, &online_users](std::string_view chunk, bool last) {
            buffer->append(chunk.data(), chunk.length());
            if (last) {
                int64_t user_id = -1;
                std::string username;
                if (token.empty() || !db.getUserByToken(token, user_id, username)) {
                    res->writeStatus("401 Unauthorized");
                    res->end("{\"error\":\"Unauthorized\"}");
                    delete buffer;
                    return;
                }

                if (online_users.count(username)) {
                    online_users.erase(username);
                    std::cout << "[Server] <<< User Left: " << username << " (ID: " << user_id << ") | Total Online Users: " << online_users.size() << std::endl;
                }
                db.deleteSession(token);

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
            std::cout << "[cim-server] Relay server with SQLite & Auth listening on port 9001..." << std::endl;
        } else {
            std::cerr << "[cim-server] Failed to listen on port 9001!" << std::endl;
        }
    }).run();

    return 0;
}
