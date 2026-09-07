#include "AdminCLI.hpp"
#include "Database.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
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
    std::string token;
};

constexpr int64_t kSessionLifetimeSeconds = 86400 * 30;

struct RegistrationWatcher {
    std::string username;
    std::string watch_token;
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

bool IsValidUsername(const std::string& username) {
    if (username.size() < 3 || username.size() > 32) {
        return false;
    }
    return std::all_of(username.begin(), username.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-' || character == '.';
    });
}

std::string NormalizeUsername(std::string username) {
    std::transform(username.begin(), username.end(), username.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return username;
}

bool IsValidRejectionReason(const std::string& reason) {
    if (reason.empty() || reason.size() > 500 ||
        std::any_of(reason.begin(), reason.end(), [](unsigned char character) {
            return character < 0x20 || character == 0x7f;
        })) {
        return false;
    }
    return std::any_of(reason.begin(), reason.end(), [](unsigned char character) {
        return !std::isspace(character);
    });
}

bool IsValidWatchToken(const std::string& token) {
    return token.size() == 64 &&
        std::all_of(token.begin(), token.end(), [](unsigned char character) {
            return std::isxdigit(character);
        });
}

} // namespace

int main(int argc, char** argv) {
    if (cim::IsAdminCommand(argc, argv)) {
        return cim::RunAdminCommand(argc, argv);
    }
    if (argc > 1) {
        std::cerr << "Unknown command. Run 'cim-server help' for usage." << std::endl;
        return 2;
    }

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
    db.cleanupExpiredRegistrationRequests();

    std::string admin_key_error;
    std::string admin_key = cim::LoadAdminKey("cim-admin.key", true, admin_key_error);
    if (admin_key.empty()) {
        std::cerr << "[Admin] " << admin_key_error << std::endl;
        ix::uninitNetSystem();
        return 1;
    }

    ix::WebSocketServer server(9001, "0.0.0.0");
    server.disablePerMessageDeflate();
    ix::WebSocketServer admin_server(9002, "127.0.0.1");
    admin_server.disablePerMessageDeflate();

    std::mutex sessions_mutex;
    std::unordered_map<ix::WebSocket*, Session> sessions;
    std::mutex registration_watchers_mutex;
    std::unordered_map<ix::WebSocket*, RegistrationWatcher> registration_watchers;
    std::mutex registration_attempts_mutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> registration_attempts;

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

    auto takeUserClients = [&](int64_t user_id) {
        std::vector<std::shared_ptr<ix::WebSocket>> clients;
        for (const auto& socket : server.getClients()) {
            auto session = sessions.find(socket.get());
            if (session != sessions.end() && session->second.user_id == user_id) {
                clients.push_back(socket);
                sessions.erase(session);
            }
        }
        return clients;
    };

    auto takeRegistrationRecipients = [&](const cim::RegistrationRequest& registration) {
        std::vector<std::shared_ptr<ix::WebSocket>> recipients;
        const std::string normalized_username = NormalizeUsername(registration.username);
        auto connected = server.getClients();
        for (const auto& socket : connected) {
            auto watcher = registration_watchers.find(socket.get());
            if (watcher != registration_watchers.end() &&
                watcher->second.username == normalized_username &&
                watcher->second.watch_token == registration.watch_token) {
                recipients.push_back(socket);
                registration_watchers.erase(watcher);
            }
        }
        return recipients;
    };

    auto notifyRegistration = [](const std::vector<std::shared_ptr<ix::WebSocket>>& recipients,
                                 const std::string& username,
                                 const std::string& watch_token,
                                 const std::string& type,
                                 const std::string& notification,
                                 const std::string& reason) {
        json response = {
            {"type", type},
            {"username", username},
            {"watch_token", watch_token},
            {"message", notification},
        };
        if (!reason.empty()) {
            response["reason"] = reason;
        }
        const std::string payload = response.dump();
        for (const auto& socket : recipients) {
            socket->send(payload);
        }
    };

    server.setOnClientMessageCallback(
        [&](std::shared_ptr<ix::ConnectionState> connection_state,
            ix::WebSocket& socket,
            const ix::WebSocketMessagePtr& message) {
            if (message->type == ix::WebSocketMessageType::Open) {
                std::cout << "[WebSocket] Client connected" << std::endl;
                return;
            }
            if (message->type == ix::WebSocketMessageType::Close) {
                std::string username;
                {
                    std::lock_guard lock(registration_watchers_mutex);
                    registration_watchers.erase(&socket);
                }
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
                        if (!IsValidUsername(username)) {
                            sendError("Username must be 3-32 characters using letters, numbers, '.', '_' or '-'");
                            return;
                        }
                        if (password.size() > 256) {
                            sendError("Password must not exceed 256 characters");
                            return;
                        }
                        const std::string watch_token = request.value("watch_token", "");
                        if (!IsValidWatchToken(watch_token)) {
                            sendError("Invalid registration watch token");
                            return;
                        }
                        const auto now = std::chrono::steady_clock::now();
                        {
                            std::lock_guard lock(registration_attempts_mutex);
                            for (auto attempt = registration_attempts.begin();
                                 attempt != registration_attempts.end();) {
                                if (now - attempt->second >= std::chrono::hours(1)) {
                                    attempt = registration_attempts.erase(attempt);
                                } else {
                                    ++attempt;
                                }
                            }
                            auto previous = registration_attempts.find(connection_state->getRemoteIp());
                            if (previous != registration_attempts.end() &&
                                now - previous->second < std::chrono::seconds(3)) {
                                sendError("Please wait before submitting another registration request");
                                return;
                            }
                            registration_attempts.insert_or_assign(connection_state->getRemoteIp(), now);
                        }
                        std::string existing_password_hash;
                        if (db.getUser(username, user_id, existing_password_hash)) {
                            sendError("Username already exists");
                            return;
                        }
                        if (db.isRegistrationPending(username)) {
                            sendError("Registration request is already awaiting approval");
                            return;
                        }
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
                        cim::RegistrationRequestResult registration;
                        {
                            std::lock_guard lock(registration_watchers_mutex);
                            registration = db.submitRegistration(username, password_hash, watch_token);
                            if (registration == cim::RegistrationRequestResult::Submitted) {
                                cim::RegistrationRequest stored_request;
                                if (db.getRegistrationRequest(username, stored_request)) {
                                    registration_watchers.insert_or_assign(
                                        &socket,
                                        RegistrationWatcher{
                                            NormalizeUsername(stored_request.username),
                                            stored_request.watch_token,
                                        });
                                }
                                socket.send(json{
                                    {"type", "registration_pending"},
                                    {"username", username},
                                    {"watch_token", watch_token},
                                    {"message", "Registration submitted; wait for administrator approval, then sign in"},
                                }.dump());
                            }
                        }
                        if (registration == cim::RegistrationRequestResult::UsernameExists) {
                            sendError("Username already exists");
                            return;
                        }
                        if (registration == cim::RegistrationRequestResult::AlreadyPending) {
                            sendError("Registration request is already awaiting approval");
                            return;
                        }
                        if (registration == cim::RegistrationRequestResult::QueueFull) {
                            sendError("Registration queue is full; contact the administrator");
                            return;
                        }
                        if (registration != cim::RegistrationRequestResult::Submitted) {
                            sendError("Unable to submit registration request");
                            return;
                        }
                        std::cout << "[Server] Registration awaiting approval: " << username << std::endl;
                        return;
                    }

                    std::string password_hash;
                    if (!db.getUser(username, user_id, password_hash)) {
                        if (db.isRegistrationPending(username)) {
                            sendError("Registration is awaiting administrator approval");
                        } else {
                            sendError("Invalid username or password");
                        }
                        return;
                    }
                    if (crypto_pwhash_str_verify(
                            password_hash.c_str(), password.c_str(), password.size()) != 0) {
                        sendError("Invalid username or password");
                        return;
                    }

                    std::string token = GenerateToken();
                    {
                        std::lock_guard lock(sessions_mutex);
                        int64_t current_user_id = -1;
                        std::string current_password_hash;
                        if (!db.getUser(username, current_user_id, current_password_hash) ||
                            current_user_id != user_id || current_password_hash != password_hash) {
                            sendError("Account changed during sign in; try again");
                            return;
                        }
                        if (!db.createSession(
                                token, user_id, std::time(nullptr) + kSessionLifetimeSeconds)) {
                            sendError("Unable to create session");
                            return;
                        }
                        sessions.insert_or_assign(&socket, Session{user_id, username, token});
                    }
                    {
                        std::lock_guard lock(registration_watchers_mutex);
                        registration_watchers.erase(&socket);
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
                    bool resumed = false;
                    {
                        std::lock_guard lock(sessions_mutex);
                        if (!token.empty()) {
                            resumed = db.resumeSession(
                                token,
                                std::time(nullptr) + kSessionLifetimeSeconds,
                                user_id,
                                username);
                        }
                        if (resumed) {
                            sessions.insert_or_assign(&socket, Session{user_id, username, token});
                        } else if (!token.empty()) {
                            db.deleteSession(token);
                        }
                    }
                    if (!resumed) {
                        socket.send(json{
                            {"type", "session_invalid"},
                            {"message", "Session expired; sign in again"},
                        }.dump());
                        return;
                    }
                    {
                        std::lock_guard lock(registration_watchers_mutex);
                        registration_watchers.erase(&socket);
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

                if (type == "registration_watch") {
                    std::string username = request.value("username", "");
                    std::string watch_token = request.value("watch_token", "");
                    if (!IsValidUsername(username) || !IsValidWatchToken(watch_token)) {
                        sendError("Invalid registration username");
                        return;
                    }
                    {
                        std::lock_guard lock(registration_watchers_mutex);
                        cim::RegistrationRequest registration;
                        if (db.getRegistrationRequest(username, registration) &&
                            registration.watch_token.size() == watch_token.size() &&
                            sodium_memcmp(
                                registration.watch_token.data(),
                                watch_token.data(),
                                watch_token.size()) == 0) {
                            registration_watchers.insert_or_assign(
                                &socket,
                                RegistrationWatcher{
                                    NormalizeUsername(registration.username),
                                    registration.watch_token,
                                });
                            socket.send(json{
                                {"type", "registration_pending"},
                                {"username", registration.username},
                                {"watch_token", registration.watch_token},
                                {"message", "Registration is awaiting administrator approval"},
                            }.dump());
                            return;
                        }

                        registration_watchers.erase(&socket);
                        if (db.isRegistrationApproved(username, watch_token)) {
                            socket.send(json{
                                {"type", "registration_approved"},
                                {"username", username},
                                {"watch_token", watch_token},
                                {"message", "Registration approved. Sign in to continue"},
                            }.dump());
                        } else {
                            std::string reason;
                            const bool was_rejected = db.getRegistrationRejection(
                                username, watch_token, reason);
                            socket.send(json{
                                {"type", "registration_rejected"},
                                {"username", username},
                                {"watch_token", watch_token},
                                {"message", was_rejected
                                    ? "Registration rejected: " + reason
                                    : "Registration is no longer pending; you may submit a new request"},
                                {"reason", was_rejected ? reason : ""},
                            }.dump());
                        }
                    }
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
                if (type == "logout") {
                    std::vector<std::shared_ptr<ix::WebSocket>> revoked_clients;
                    auto connected = server.getClients();
                    {
                        std::lock_guard lock(sessions_mutex);
                        if (!db.deleteSession(sender.token)) {
                            sendError("Unable to revoke session");
                            return;
                        }
                        for (const auto& client : connected) {
                            auto session = sessions.find(client.get());
                            if (session != sessions.end() && session->second.token == sender.token) {
                                if (client.get() != &socket) {
                                    revoked_clients.push_back(client);
                                }
                                sessions.erase(session);
                            }
                        }
                    }
                    socket.send(json{{"type", "logout_success"}}.dump());
                    for (const auto& client : revoked_clients) {
                        client->close(4001, "Session signed out");
                    }
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
                    {"sent_at", static_cast<int64_t>(std::time(nullptr))},
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

    admin_server.setOnClientMessageCallback(
        [&](std::shared_ptr<ix::ConnectionState>,
            ix::WebSocket& socket,
            const ix::WebSocketMessagePtr& message) {
            if (message->type != ix::WebSocketMessageType::Message) {
                return;
            }

            auto sendError = [&socket](std::string_view error) {
                socket.send(json{{"success", false}, {"error", error}}.dump());
            };
            auto request = json::parse(message->str, nullptr, false);
            if (request.is_discarded() || !request.is_object() ||
                !request.contains("token") || !request["token"].is_string() ||
                !request.contains("command") || !request["command"].is_string()) {
                sendError("Invalid administrator request");
                return;
            }
            std::string supplied_key = request["token"].get<std::string>();
            if (supplied_key.size() != admin_key.size() ||
                sodium_memcmp(supplied_key.data(), admin_key.data(), admin_key.size()) != 0) {
                sendError("Administrator authentication failed");
                return;
            }

            std::string command = request["command"].get<std::string>();
            if (command == "pending") {
                std::vector<cim::RegistrationRequest> registrations;
                if (!db.getRegistrationRequests(registrations)) {
                    sendError("Unable to list registration requests");
                    return;
                }
                json requests = json::array();
                for (const auto& registration : registrations) {
                    requests.push_back({
                        {"username", registration.username},
                        {"requested_at", registration.requested_at},
                    });
                }
                socket.send(json{
                    {"success", true},
                    {"command", "pending"},
                    {"requests", requests},
                }.dump());
                return;
            }
            if (command == "users") {
                std::unordered_set<int64_t> online_ids;
                for (const auto& [client, session] : authenticatedClients()) {
                    online_ids.insert(session.user_id);
                }
                json users = json::array();
                for (const auto& user : db.getUserRecords()) {
                    users.push_back({
                        {"id", user.id},
                        {"username", user.username},
                        {"created_at", user.created_at},
                        {"online", online_ids.contains(user.id)},
                    });
                }
                socket.send(json{
                    {"success", true},
                    {"command", "users"},
                    {"users", users},
                }.dump());
                return;
            }

            if (!request.contains("username") || !request["username"].is_string()) {
                sendError("Username is required");
                return;
            }
            std::string username = request["username"].get<std::string>();
            if (username.empty()) {
                sendError("Username is required");
                return;
            }
            if (command == "approve") {
                int64_t user_id = -1;
                cim::RegistrationRequest registration;
                std::vector<std::shared_ptr<ix::WebSocket>> recipients;
                {
                    std::lock_guard lock(registration_watchers_mutex);
                    if (!db.getRegistrationRequest(username, registration) ||
                        !db.approveRegistration(username, user_id)) {
                        sendError("Registration request not found or approval failed");
                        return;
                    }
                    recipients = takeRegistrationRecipients(registration);
                }
                notifyRegistration(
                    recipients,
                    registration.username,
                    registration.watch_token,
                    "registration_approved",
                    "Registration approved. Sign in to continue",
                    "");
                broadcastUsers();
                socket.send(json{
                    {"success", true},
                    {"message", "Registration approved"},
                }.dump());
                return;
            }
            if (command == "reject") {
                if (!request.contains("reason") || !request["reason"].is_string() ||
                    !IsValidRejectionReason(request["reason"].get_ref<const std::string&>())) {
                    sendError("Rejection reason is required and must be at most 500 bytes without control characters");
                    return;
                }
                const std::string reason = request["reason"].get<std::string>();
                cim::RegistrationRequest registration;
                std::vector<std::shared_ptr<ix::WebSocket>> recipients;
                {
                    std::lock_guard lock(registration_watchers_mutex);
                    if (!db.getRegistrationRequest(username, registration) ||
                        !db.rejectRegistration(username, reason)) {
                        sendError("Registration request not found or rejection failed");
                        return;
                    }
                    recipients = takeRegistrationRecipients(registration);
                }
                notifyRegistration(
                    recipients,
                    registration.username,
                    registration.watch_token,
                    "registration_rejected",
                    "Registration rejected: " + reason,
                    reason);
                socket.send(json{
                    {"success", true},
                    {"message", "Registration rejected"},
                }.dump());
                return;
            }
            if (command == "passwd") {
                if (!request.contains("password") || !request["password"].is_string() ||
                    request["password"].get_ref<const std::string&>().empty()) {
                    sendError("Password cannot be empty");
                    return;
                }
                std::string password = request["password"].get<std::string>();
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
                int64_t user_id = -1;
                std::vector<std::shared_ptr<ix::WebSocket>> revoked_clients;
                {
                    std::lock_guard lock(sessions_mutex);
                    if (!db.changePassword(username, password_hash, user_id)) {
                        sendError("User not found or password update failed");
                        return;
                    }
                    revoked_clients = takeUserClients(user_id);
                }
                for (const auto& client : revoked_clients) {
                    client->close(4001, "Password changed by administrator");
                }
                broadcastUsers();
                socket.send(json{
                    {"success", true},
                    {"message", "Password changed; existing sessions were revoked"},
                }.dump());
                return;
            }
            if (command == "delete") {
                int64_t user_id = -1;
                std::vector<std::shared_ptr<ix::WebSocket>> revoked_clients;
                {
                    std::lock_guard lock(sessions_mutex);
                    if (!db.deleteUser(username, user_id)) {
                        sendError("User not found or deletion failed");
                        return;
                    }
                    revoked_clients = takeUserClients(user_id);
                }
                for (const auto& client : revoked_clients) {
                    client->close(4001, "Account deleted by administrator");
                }
                broadcastUsers();
                socket.send(json{
                    {"success", true},
                    {"message", "User deleted"},
                }.dump());
                return;
            }
            sendError("Unsupported administrator command");
        }
    );

    auto [listening, error] = server.listen();
    if (!listening) {
        std::cerr << "[cim-server] Failed to listen on port 9001: " << error << std::endl;
        ix::uninitNetSystem();
        return 1;
    }

    auto [admin_listening, admin_error] = admin_server.listen();
    if (!admin_listening) {
        std::cerr << "[Admin] Failed to listen on 127.0.0.1:9002: "
                  << admin_error << std::endl;
        ix::uninitNetSystem();
        return 1;
    }

    std::cout << "[cim-server] IXWebSocket server listening on port 9001" << std::endl;
    std::cout << "[Admin] Local management listening on 127.0.0.1:9002" << std::endl;
    server.start();
    admin_server.start();
    server.wait();
    admin_server.stop();
    ix::uninitNetSystem();
    return 0;
}
