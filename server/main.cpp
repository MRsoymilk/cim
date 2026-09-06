#include "Database.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace {

struct Session {
    int64_t user_id;
    std::string username;
};

std::string GenerateToken() {
    unsigned char bytes[32];
    randombytes_buf(bytes, sizeof(bytes));
    std::stringstream stream;
    for (unsigned char byte : bytes) {
        stream << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(byte);
    }
    return stream.str();
}

} // namespace

int main() {
    if (sodium_init() < 0) {
        std::cerr << "[Server] Failed to initialize libsodium" << std::endl;
        return 1;
    }
    if (!ix::initNetSystem()) {
        std::cerr << "[Server] Failed to initialize network system" << std::endl;
        return 1;
    }

    cim::Database db;
    if (!db.init("cim.db")) {
        ix::uninitNetSystem();
        return 1;
    }
    db.cleanupExpiredSessions();

    ix::WebSocketServer server(9001, "0.0.0.0");
    server.disablePerMessageDeflate();

    std::mutex sessions_mutex;
    std::unordered_map<ix::WebSocket*, Session> sessions;

    auto authenticatedClients = [&] {
        std::vector<std::pair<std::shared_ptr<ix::WebSocket>, Session>> clients;
        auto connected = server.getClients();
        std::lock_guard lock(sessions_mutex);
        for (const auto& socket : connected) {
            auto session = sessions.find(socket.get());
            if (session != sessions.end()) {
                clients.emplace_back(socket, session->second);
            }
        }
        return clients;
    };

    auto broadcastUsers = [&] {
        auto clients = authenticatedClients();
        std::unordered_set<int64_t> online_ids;
        for (const auto& [socket, session] : clients) {
            online_ids.insert(session.user_id);
        }

        json users = json::array();
        for (const auto& [user_id, username] : db.getAllUsers()) {
            users.push_back({
                {"id", user_id},
                {"username", username},
                {"online", online_ids.contains(user_id)},
            });
        }
        std::string payload = json{{"type", "users"}, {"users", users}}.dump();
        for (const auto& [socket, session] : clients) {
            socket->send(payload);
        }
    };

    server.setOnClientMessageCallback(
        [&](std::shared_ptr<ix::ConnectionState>,
            ix::WebSocket& socket,
            const ix::WebSocketMessagePtr& message) {
            if (message->type == ix::WebSocketMessageType::Open) {
                std::cout << "[WebSocket] Client connected" << std::endl;
                return;
            }
            if (message->type == ix::WebSocketMessageType::Close) {
                std::string username;
                {
                    std::lock_guard lock(sessions_mutex);
                    auto session = sessions.find(&socket);
                    if (session != sessions.end()) {
                        username = session->second.username;
                        sessions.erase(session);
                    }
                }
                if (!username.empty()) {
                    std::cout << "[WebSocket] User disconnected: " << username << std::endl;
                    broadcastUsers();
                }
                return;
            }
            if (message->type != ix::WebSocketMessageType::Message) {
                return;
            }

            auto sendError = [&socket](std::string_view error) {
                socket.send(json{{"type", "error"}, {"error", error}}.dump());
            };

            try {
                auto request = json::parse(message->str);
                std::string type = request.value("type", "");

                if (type == "login" || type == "register") {
                    std::string username = request.value("username", "");
                    std::string password = request.value("password", "");
                    if (username.empty() || password.empty()) {
                        sendError("Username and password are required");
                        return;
                    }

                    int64_t user_id = -1;
                    if (type == "register") {
                        char password_hash[crypto_pwhash_STRBYTES];
                        if (crypto_pwhash_str(
                                password_hash,
                                password.c_str(),
                                password.size(),
                                crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
                            sendError("Password hashing failed");
                            return;
                        }
                        user_id = db.registerUser(username, password_hash);
                        if (user_id < 0) {
                            sendError("Username already exists");
                            return;
                        }
                    } else {
                        std::string password_hash;
                        if (!db.getUser(username, user_id, password_hash) ||
                            crypto_pwhash_str_verify(
                                password_hash.c_str(), password.c_str(), password.size()) != 0) {
                            sendError("Invalid username or password");
                            return;
                        }
                    }

                    std::string token = GenerateToken();
                    db.createSession(token, user_id, std::time(nullptr) + 86400 * 7);
                    {
                        std::lock_guard lock(sessions_mutex);
                        sessions.insert_or_assign(&socket, Session{user_id, username});
                    }
                    socket.send(json{
                        {"type", "auth_success"},
                        {"token", token},
                        {"user_id", user_id},
                        {"username", username},
                    }.dump());
                    std::cout << "[Server] Authenticated: " << username
                              << " (ID: " << user_id << ")" << std::endl;
                    broadcastUsers();
                    return;
                }

                if (type == "resume") {
                    int64_t user_id = -1;
                    std::string username;
                    std::string token = request.value("token", "");
                    if (token.empty() || !db.getUserByToken(token, user_id, username)) {
                        sendError("Session expired; sign in again");
                        return;
                    }
                    {
                        std::lock_guard lock(sessions_mutex);
                        sessions.insert_or_assign(&socket, Session{user_id, username});
                    }
                    socket.send(json{
                        {"type", "auth_success"},
                        {"token", token},
                        {"user_id", user_id},
                        {"username", username},
                    }.dump());
                    broadcastUsers();
                    return;
                }

                Session sender;
                {
                    std::lock_guard lock(sessions_mutex);
                    auto session = sessions.find(&socket);
                    if (session == sessions.end()) {
                        sendError("Authentication required");
                        return;
                    }
                    sender = session->second;
                }

                if (type == "users") {
                    broadcastUsers();
                    return;
                }
                if (type != "chat") {
                    sendError("Unsupported message type");
                    return;
                }

                int64_t recipient_id = request.value("recipient_id", int64_t{-1});
                std::string content = request.value("content", "");
                std::string recipient_name;
                if (recipient_id < 0 || !db.getUserById(recipient_id, recipient_name)) {
                    sendError("Recipient not found");
                    return;
                }
                if (content.empty() || content.size() > 4096) {
                    sendError("Message must contain between 1 and 4096 bytes");
                    return;
                }

                std::string payload = json{
                    {"type", "chat"},
                    {"sender_id", sender.user_id},
                    {"sender_name", sender.username},
                    {"recipient_id", recipient_id},
                    {"recipient_name", recipient_name},
                    {"content", content},
                }.dump();
                for (const auto& [client, session] : authenticatedClients()) {
                    if (session.user_id == sender.user_id || session.user_id == recipient_id) {
                        client->send(payload);
                    }
                }
            } catch (const json::exception&) {
                sendError("Invalid message JSON");
            }
        }
    );

    auto [listening, error] = server.listen();
    if (!listening) {
        std::cerr << "[cim-server] Failed to listen on port 9001: " << error << std::endl;
        ix::uninitNetSystem();
        return 1;
    }

    std::cout << "[cim-server] IXWebSocket server listening on port 9001" << std::endl;
    server.start();
    server.wait();
    ix::uninitNetSystem();
    return 0;
}
