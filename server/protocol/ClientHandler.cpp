#include "core/CimServer.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <chrono>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace cim {
namespace {

using json = nlohmann::json;
constexpr int64_t kSessionLifetimeSeconds = 86400 * 30;

} // namespace

void CimServer::ConfigureClientHandler() {
    public_server_->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> connection_state,
               ix::WebSocket& socket,
               const ix::WebSocketMessagePtr& message) {
            HandleClientMessage(std::move(connection_state), socket, message);
        });
}

void CimServer::HandleClientMessage(
    std::shared_ptr<ix::ConnectionState> connection_state,
    ix::WebSocket& socket,
    const ix::WebSocketMessagePtr& message) {
    if (message->type == ix::WebSocketMessageType::Open) {
        std::cout << "[WebSocket] Client connected" << std::endl;
        return;
    }
    if (message->type == ix::WebSocketMessageType::Close) {
        std::string username;
        {
            std::lock_guard lock(registration_watchers_mutex_);
            registration_watchers_.erase(&socket);
        }
        {
            std::lock_guard lock(sessions_mutex_);
            auto session = sessions_.find(&socket);
            if (session != sessions_.end()) {
                username = session->second.username;
                sessions_.erase(session);
            }
        }
        if (!username.empty()) {
            std::cout << "[WebSocket] User disconnected: " << username << std::endl;
            BroadcastUsers();
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
                    std::lock_guard lock(registration_attempts_mutex_);
                    for (auto attempt = registration_attempts_.begin();
                         attempt != registration_attempts_.end();) {
                        if (now - attempt->second >= std::chrono::hours(1)) {
                            attempt = registration_attempts_.erase(attempt);
                        } else {
                            ++attempt;
                        }
                    }
                    auto previous = registration_attempts_.find(connection_state->getRemoteIp());
                    if (previous != registration_attempts_.end() &&
                        now - previous->second < std::chrono::seconds(3)) {
                        sendError("Please wait before submitting another registration request");
                        return;
                    }
                    registration_attempts_.insert_or_assign(connection_state->getRemoteIp(), now);
                }
                std::string existing_password_hash;
                if (db_.getUser(username, user_id, existing_password_hash)) {
                    sendError("Username already exists");
                    return;
                }
                if (db_.isRegistrationPending(username)) {
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
                RegistrationRequestResult registration;
                {
                    std::lock_guard lock(registration_watchers_mutex_);
                    registration = db_.submitRegistration(username, password_hash, watch_token);
                    if (registration == RegistrationRequestResult::Submitted) {
                        RegistrationRequest stored_request;
                        if (db_.getRegistrationRequest(username, stored_request)) {
                            registration_watchers_.insert_or_assign(
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
                if (registration == RegistrationRequestResult::UsernameExists) {
                    sendError("Username already exists");
                    return;
                }
                if (registration == RegistrationRequestResult::AlreadyPending) {
                    sendError("Registration request is already awaiting approval");
                    return;
                }
                if (registration == RegistrationRequestResult::QueueFull) {
                    sendError("Registration queue is full; contact the administrator");
                    return;
                }
                if (registration != RegistrationRequestResult::Submitted) {
                    sendError("Unable to submit registration request");
                    return;
                }
                std::cout << "[Server] Registration awaiting approval: " << username << std::endl;
                return;
            }

            std::string password_hash;
            if (!db_.getUser(username, user_id, password_hash)) {
                if (db_.isRegistrationPending(username)) {
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
                std::lock_guard lock(sessions_mutex_);
                int64_t current_user_id = -1;
                std::string current_password_hash;
                if (!db_.getUser(username, current_user_id, current_password_hash) ||
                    current_user_id != user_id || current_password_hash != password_hash) {
                    sendError("Account changed during sign in; try again");
                    return;
                }
                if (!db_.createSession(
                        token, user_id, std::time(nullptr) + kSessionLifetimeSeconds)) {
                    sendError("Unable to create session");
                    return;
                }
                sessions_.insert_or_assign(&socket, Session{user_id, username, token});
            }
            {
                std::lock_guard lock(registration_watchers_mutex_);
                registration_watchers_.erase(&socket);
            }
            socket.send(json{
                {"type", "auth_success"},
                {"token", token},
                {"user_id", user_id},
                {"username", username},
            }.dump());
            std::cout << "[Server] Authenticated: " << username
                      << " (ID: " << user_id << ")" << std::endl;
            BroadcastUsers();
            return;
        }

        if (type == "resume") {
            int64_t user_id = -1;
            std::string username;
            std::string token = request.value("token", "");
            bool resumed = false;
            {
                std::lock_guard lock(sessions_mutex_);
                if (!token.empty()) {
                    resumed = db_.resumeSession(
                        token,
                        std::time(nullptr) + kSessionLifetimeSeconds,
                        user_id,
                        username);
                }
                if (resumed) {
                    sessions_.insert_or_assign(&socket, Session{user_id, username, token});
                } else if (!token.empty()) {
                    db_.deleteSession(token);
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
                std::lock_guard lock(registration_watchers_mutex_);
                registration_watchers_.erase(&socket);
            }
            socket.send(json{
                {"type", "auth_success"},
                {"token", token},
                {"user_id", user_id},
                {"username", username},
            }.dump());
            BroadcastUsers();
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
                std::lock_guard lock(registration_watchers_mutex_);
                RegistrationRequest registration;
                if (db_.getRegistrationRequest(username, registration) &&
                    registration.watch_token.size() == watch_token.size() &&
                    sodium_memcmp(
                        registration.watch_token.data(),
                        watch_token.data(),
                        watch_token.size()) == 0) {
                    registration_watchers_.insert_or_assign(
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

                registration_watchers_.erase(&socket);
                if (db_.isRegistrationApproved(username, watch_token)) {
                    socket.send(json{
                        {"type", "registration_approved"},
                        {"username", username},
                        {"watch_token", watch_token},
                        {"message", "Registration approved. Sign in to continue"},
                    }.dump());
                } else {
                    std::string reason;
                    const bool was_rejected = db_.getRegistrationRejection(
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
            std::lock_guard lock(sessions_mutex_);
            auto session = sessions_.find(&socket);
            if (session == sessions_.end()) {
                sendError("Authentication required");
                return;
            }
            sender = session->second;
        }

        if (type == "users") {
            BroadcastUsers();
            return;
        }
        if (type == "logout") {
            std::vector<std::shared_ptr<ix::WebSocket>> revoked_clients;
            auto connected = public_server_->getClients();
            {
                std::lock_guard lock(sessions_mutex_);
                if (!db_.deleteSession(sender.token)) {
                    sendError("Unable to revoke session");
                    return;
                }
                for (const auto& client : connected) {
                    auto session = sessions_.find(client.get());
                    if (session != sessions_.end() && session->second.token == sender.token) {
                        if (client.get() != &socket) {
                            revoked_clients.push_back(client);
                        }
                        sessions_.erase(session);
                    }
                }
            }
            socket.send(json{{"type", "logout_success"}}.dump());
            for (const auto& client : revoked_clients) {
                client->close(4001, "Session signed out");
            }
            BroadcastUsers();
            return;
        }
        if (type != "chat") {
            sendError("Unsupported message type");
            return;
        }

        int64_t recipient_id = request.value("recipient_id", int64_t{-1});
        std::string content = request.value("content", "");
        std::string recipient_name;
        if (recipient_id < 0 || !db_.getUserById(recipient_id, recipient_name)) {
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
        for (const auto& [client, session] : AuthenticatedClients()) {
            if (session.user_id == sender.user_id || session.user_id == recipient_id) {
                client->send(payload);
            }
        }
    } catch (const json::exception&) {
        sendError("Invalid message JSON");
    }
}

} // namespace cim
