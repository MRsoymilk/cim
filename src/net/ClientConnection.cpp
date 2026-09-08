#include "ClientConnection.hpp"
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocket.h>
#include <utility>

namespace cim {

namespace {

constexpr const char* kSecureCipherSuites =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";

} // namespace

ClientConnection::ClientConnection(ftxui::Closure request_refresh)
    : request_refresh_(std::move(request_refresh)) {
    ix::initNetSystem();
}

ClientConnection::~ClientConnection() {
    Stop();
    ix::uninitNetSystem();
}

void ClientConnection::Start(
    const std::string& url, const std::string& trusted_ca_data) {
    websocket_ = std::make_unique<ix::WebSocket>();
    websocket_->setUrl(url);
    ix::SocketTLSOptions tls_options;
    tls_options.caFile = trusted_ca_data;
    tls_options.ciphers = kSecureCipherSuites;
    tls_options.disable_hostname_validation = false;
    websocket_->setTLSOptions(tls_options);
    websocket_->setPingInterval(30);
    websocket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
        Event event;
        switch (message->type) {
            case ix::WebSocketMessageType::Open:
                connected_ = true;
                authenticated_ = false;
                event.type = EventType::Connected;
                break;
            case ix::WebSocketMessageType::Close:
                connected_ = false;
                authenticated_ = false;
                event.type = EventType::Disconnected;
                event.content = message->closeInfo.reason;
                break;
            case ix::WebSocketMessageType::Error:
                connected_ = false;
                authenticated_ = false;
                event.type = EventType::Error;
                event.content = message->errorInfo.reason;
                break;
            case ix::WebSocketMessageType::Message:
                event.type = EventType::Message;
                event.content = message->str;
                break;
            default:
                return;
        }

        {
            std::lock_guard lock(events_mutex_);
            events_.push_back(std::move(event));
        }
        request_refresh_();
    });
    websocket_->start();
}

void ClientConnection::Stop() {
    if (websocket_) {
        websocket_->stop();
        websocket_.reset();
    }
    {
        std::lock_guard lock(events_mutex_);
        events_.clear();
    }
    connected_ = false;
    authenticated_ = false;
}

bool ClientConnection::Send(const std::string& message) {
    return websocket_ && websocket_->send(message).success;
}

std::deque<ClientConnection::Event> ClientConnection::DrainEvents() {
    std::deque<Event> events;
    {
        std::lock_guard lock(events_mutex_);
        events.swap(events_);
    }
    return events;
}

bool ClientConnection::IsConnected() const {
    return connected_;
}

bool ClientConnection::IsAuthenticated() const {
    return authenticated_;
}

void ClientConnection::SetAuthenticated(bool authenticated) {
    authenticated_ = authenticated;
}

} // namespace cim
