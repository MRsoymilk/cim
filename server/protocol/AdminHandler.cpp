#include "core/CimServer.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace cim {
namespace {

using json = nlohmann::json;

} // namespace

void CimServer::ConfigureAdminHandler() {
    admin_server_->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState>,
               ix::WebSocket& socket,
               const ix::WebSocketMessagePtr& message) {
            HandleAdminMessage(socket, message);
        });
}

void CimServer::HandleAdminMessage(
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
    if (supplied_key.size() != admin_key_.size() ||
        sodium_memcmp(supplied_key.data(), admin_key_.data(), admin_key_.size()) != 0) {
        sendError("Administrator authentication failed");
        return;
    }

    std::string command = request["command"].get<std::string>();
    if (command == "pending") {
        std::vector<RegistrationRequest> registrations;
        if (!db_.getRegistrationRequests(registrations)) {
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
        for (const auto& [client, session] : AuthenticatedClients()) {
            online_ids.insert(session.user_id);
        }
        json users = json::array();
        for (const auto& user : db_.getUserRecords()) {
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
        RegistrationRequest registration;
        std::vector<std::shared_ptr<ix::WebSocket>> recipients;
        {
            std::lock_guard lock(registration_watchers_mutex_);
            if (!db_.getRegistrationRequest(username, registration) ||
                !db_.approveRegistration(username, user_id)) {
                sendError("Registration request not found or approval failed");
                return;
            }
            recipients = TakeRegistrationRecipientsLocked(registration);
        }
        NotifyRegistration(
            recipients,
            registration.username,
            registration.watch_token,
            "registration_approved",
            "Registration approved. Sign in to continue",
            "");
        BroadcastUsers();
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
        RegistrationRequest registration;
        std::vector<std::shared_ptr<ix::WebSocket>> recipients;
        {
            std::lock_guard lock(registration_watchers_mutex_);
            if (!db_.getRegistrationRequest(username, registration) ||
                !db_.rejectRegistration(username, reason)) {
                sendError("Registration request not found or rejection failed");
                return;
            }
            recipients = TakeRegistrationRecipientsLocked(registration);
        }
        NotifyRegistration(
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
            std::lock_guard lock(sessions_mutex_);
            if (!db_.changePassword(username, password_hash, user_id)) {
                sendError("User not found or password update failed");
                return;
            }
            revoked_clients = TakeUserClientsLocked(user_id);
        }
        for (const auto& client : revoked_clients) {
            client->close(4001, "Password changed by administrator");
        }
        BroadcastUsers();
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
            std::lock_guard lock(sessions_mutex_);
            if (!db_.deleteUser(username, user_id)) {
                sendError("User not found or deletion failed");
                return;
            }
            revoked_clients = TakeUserClientsLocked(user_id);
        }
        for (const auto& client : revoked_clients) {
            client->close(4001, "Account deleted by administrator");
        }
        BroadcastUsers();
        socket.send(json{
            {"success", true},
            {"message", "User deleted"},
        }.dump());
        return;
    }
    sendError("Unsupported administrator command");
}

} // namespace cim
