#include "core/CimServer.hpp"
#include "admin/AdminCLI.hpp"

#include <ixwebsocket/IXSocketTLSOptions.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

namespace cim {
namespace {

using json = nlohmann::json;

} // namespace

CimServer::CimServer(ServerOptions options)
    : options_(std::move(options)) {}

CimServer::~CimServer() {
    if (admin_server_) {
        admin_server_->stop();
    }
    if (public_server_) {
        public_server_->stop();
    }
    admin_server_.reset();
    public_server_.reset();
}

bool CimServer::Initialize() {
    if (!db_.init("cim.db")) {
        return false;
    }
    db_.cleanupExpiredSessions();
    db_.cleanupExpiredRegistrationRequests();

    std::string admin_key_error;
    admin_key_ = LoadAdminKey("cim-admin.key", true, admin_key_error);
    if (admin_key_.empty()) {
        std::cerr << "[Admin] " << admin_key_error << std::endl;
        return false;
    }

    public_server_ = std::make_unique<ix::WebSocketServer>(9001, "0.0.0.0");
    public_server_->disablePerMessageDeflate();
    ix::SocketTLSOptions tls_options;
    tls_options.tls = true;
    tls_options.certFile = options_.certificate.string();
    tls_options.keyFile = options_.private_key.string();
    tls_options.caFile = "NONE";
    tls_options.ciphers = kSecureCipherSuites;
    tls_options.disable_hostname_validation = false;
    public_server_->setTLSOptions(tls_options);

    admin_server_ = std::make_unique<ix::WebSocketServer>(9002, "127.0.0.1");
    admin_server_->disablePerMessageDeflate();

    ConfigureClientHandler();
    ConfigureAdminHandler();

    auto [listening, error] = public_server_->listen();
    if (!listening) {
        std::cerr << "[cim-server] Failed to listen on port 9001: " << error << std::endl;
        return false;
    }

    auto [admin_listening, admin_error] = admin_server_->listen();
    if (!admin_listening) {
        std::cerr << "[Admin] Failed to listen on 127.0.0.1:9002: "
                  << admin_error << std::endl;
        return false;
    }
    return true;
}

int CimServer::Run(const std::function<bool()>& stop_requested) {
    std::cout << "[cim-server] Secure WebSocket server listening on port 9001" << std::endl;
    std::cout << "[Admin] Local management listening on 127.0.0.1:9002" << std::endl;
    public_server_->start();
    admin_server_->start();

    while (!stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[cim-server] Shutting down" << std::endl;
    admin_server_->stop();
    public_server_->stop();
    return 0;
}

std::vector<CimServer::AuthenticatedClient> CimServer::AuthenticatedClients() {
    std::vector<AuthenticatedClient> clients;
    auto connected = public_server_->getClients();
    std::lock_guard lock(sessions_mutex_);
    for (const auto& socket : connected) {
        auto session = sessions_.find(socket.get());
        if (session != sessions_.end()) {
            clients.emplace_back(socket, session->second);
        }
    }
    return clients;
}

void CimServer::BroadcastUsers() {
    auto clients = AuthenticatedClients();
    std::unordered_set<int64_t> online_ids;
    for (const auto& [socket, session] : clients) {
        online_ids.insert(session.user_id);
    }

    json users = json::array();
    for (const auto& [user_id, username] : db_.getAllUsers()) {
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
}

std::vector<std::shared_ptr<ix::WebSocket>> CimServer::TakeUserClientsLocked(int64_t user_id) {
    std::vector<std::shared_ptr<ix::WebSocket>> clients;
    for (const auto& socket : public_server_->getClients()) {
        auto session = sessions_.find(socket.get());
        if (session != sessions_.end() && session->second.user_id == user_id) {
            clients.push_back(socket);
            sessions_.erase(session);
        }
    }
    return clients;
}

std::vector<std::shared_ptr<ix::WebSocket>> CimServer::TakeRegistrationRecipientsLocked(
    const RegistrationRequest& registration) {
    std::vector<std::shared_ptr<ix::WebSocket>> recipients;
    const std::string normalized_username = NormalizeUsername(registration.username);
    auto connected = public_server_->getClients();
    for (const auto& socket : connected) {
        auto watcher = registration_watchers_.find(socket.get());
        if (watcher != registration_watchers_.end() &&
            watcher->second.username == normalized_username &&
            watcher->second.watch_token == registration.watch_token) {
            recipients.push_back(socket);
            registration_watchers_.erase(watcher);
        }
    }
    return recipients;
}

void CimServer::NotifyRegistration(
    const std::vector<std::shared_ptr<ix::WebSocket>>& recipients,
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
}

std::string CimServer::GenerateToken() {
    unsigned char bytes[32];
    randombytes_buf(bytes, sizeof(bytes));
    std::stringstream stream;
    for (unsigned char byte : bytes) {
        stream << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(byte);
    }
    return stream.str();
}

bool CimServer::IsValidUsername(const std::string& username) {
    if (username.size() < 3 || username.size() > 32) {
        return false;
    }
    return std::all_of(username.begin(), username.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-' || character == '.';
    });
}

std::string CimServer::NormalizeUsername(std::string username) {
    std::transform(username.begin(), username.end(), username.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return username;
}

bool CimServer::IsValidRejectionReason(const std::string& reason) {
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

bool CimServer::IsValidWatchToken(const std::string& token) {
    return token.size() == 64 &&
        std::all_of(token.begin(), token.end(), [](unsigned char character) {
            return std::isxdigit(character);
        });
}

} // namespace cim
