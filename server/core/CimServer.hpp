#pragma once

#include "config/ServerOptions.hpp"
#include "storage/Database.hpp"

#include <ixwebsocket/IXWebSocketServer.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cim {

class CimServer {
public:
    explicit CimServer(ServerOptions options);
    ~CimServer();

    bool Initialize();
    int Run();

private:
    struct Session {
        int64_t user_id;
        std::string username;
        std::string token;
    };

    struct RegistrationWatcher {
        std::string username;
        std::string watch_token;
    };

    using AuthenticatedClient = std::pair<std::shared_ptr<ix::WebSocket>, Session>;

    void ConfigureClientHandler();
    void ConfigureAdminHandler();
    void HandleClientMessage(std::shared_ptr<ix::ConnectionState> connection_state,
                             ix::WebSocket& socket,
                             const ix::WebSocketMessagePtr& message);
    void HandleAdminMessage(ix::WebSocket& socket,
                            const ix::WebSocketMessagePtr& message);

    std::vector<AuthenticatedClient> AuthenticatedClients();
    void BroadcastUsers();
    std::vector<std::shared_ptr<ix::WebSocket>> TakeUserClientsLocked(int64_t user_id);
    std::vector<std::shared_ptr<ix::WebSocket>> TakeRegistrationRecipientsLocked(
        const RegistrationRequest& registration);
    static void NotifyRegistration(
        const std::vector<std::shared_ptr<ix::WebSocket>>& recipients,
        const std::string& username,
        const std::string& watch_token,
        const std::string& type,
        const std::string& notification,
        const std::string& reason);
    static std::string GenerateToken();
    static bool IsValidUsername(const std::string& username);
    static std::string NormalizeUsername(std::string username);
    static bool IsValidRejectionReason(const std::string& reason);
    static bool IsValidWatchToken(const std::string& token);

    Database db_;
    ServerOptions options_;
    std::string admin_key_;
    std::mutex sessions_mutex_;
    std::unordered_map<ix::WebSocket*, Session> sessions_;
    std::mutex registration_watchers_mutex_;
    std::unordered_map<ix::WebSocket*, RegistrationWatcher> registration_watchers_;
    std::mutex registration_attempts_mutex_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> registration_attempts_;

    // Destroyed first so callback threads cannot access already-destroyed state.
    std::unique_ptr<ix::WebSocketServer> public_server_;
    std::unique_ptr<ix::WebSocketServer> admin_server_;
};

} // namespace cim
