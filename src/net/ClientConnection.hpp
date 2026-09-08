#pragma once

#include "ftxui/component/task.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace ix {
class WebSocket;
}

namespace cim {

class ClientConnection {
public:
    enum class EventType {
        Connected,
        Disconnected,
        Message,
        Error
    };

    struct Event {
        EventType type;
        std::string content;
    };

    explicit ClientConnection(ftxui::Closure request_refresh);
    ~ClientConnection();

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    void Start(const std::string& url, const std::string& trusted_ca_data);
    void Stop();
    bool Send(const std::string& message);
    std::deque<Event> DrainEvents();

    bool IsConnected() const;
    bool IsAuthenticated() const;
    void SetAuthenticated(bool authenticated);

private:
    ftxui::Closure request_refresh_;
    std::unique_ptr<ix::WebSocket> websocket_;
    std::atomic<bool> connected_ = false;
    std::atomic<bool> authenticated_ = false;
    std::mutex events_mutex_;
    std::deque<Event> events_;
};

} // namespace cim
