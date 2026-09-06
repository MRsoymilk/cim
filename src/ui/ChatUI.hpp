#pragma once

#include "ftxui/component/component.hpp"
#include "ftxui/component/task.hpp"
#include <atomic>
#include <string>
#include <vector>
#include <thread>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace ix {
class WebSocket;
}

namespace cim {

struct Contact {
    int64_t id;
    std::string name;
    bool online;
    std::vector<std::pair<bool, std::string>> messages;
};

enum class AuthState {
    Login,
    Register,
    LoggedIn
};

class ChatUI {
public:
    explicit ChatUI(ftxui::Closure request_refresh);
    ~ChatUI();
    ftxui::Component GetComponent();

private:
    enum class SocketEventType {
        Connected,
        Disconnected,
        Chat,
        Error
    };

    struct SocketEvent {
        SocketEventType type;
        int64_t sender_id = -1;
        std::string sender_name;
        int64_t recipient_id = -1;
        std::string recipient_name;
        std::string content;
    };

    ftxui::Closure request_refresh_;
    AuthState auth_state_ = AuthState::Login;
    std::string auth_username_ = "";
    std::string auth_password_ = "";
    std::string auth_error_ = "";
    std::string auth_token_ = "";
    std::string auth_action_label_ = "SIGN IN";
    std::string auth_switch_label_ = "New here? Create an account";
    int64_t my_user_id_ = -1;
    std::string my_name_ = "";

    bool editing_name_ = false;
    std::string name_input_text_ = "";
    std::string search_query_ = "";
    
    std::vector<Contact> contacts_;
    int selected_contact_index_ = 0;

    std::string chat_input_text_ = "";
    std::string notification_;

    int active_pane_ = 0; // 0 = left, 1 = right
    int active_tab_index_ = 0; // 0 = login, 1 = chat

    ftxui::Component login_username_input_;
    ftxui::Component login_password_input_;
    ftxui::Component login_btn_;
    ftxui::Component switch_btn_;
    ftxui::Component my_name_input_;
    ftxui::Component search_input_;
    ftxui::Component chat_input_;
    ftxui::Component name_button_;
    ftxui::Component contact_menu_;
    ftxui::Component left_container_;
    ftxui::Component right_container_;
    ftxui::Component split_container_;
    ftxui::Component login_container_;
    ftxui::Component root_container_;

    std::vector<int> filtered_indices_;
    std::vector<std::string> filtered_names_;

    std::jthread heartbeat_thread_;
    std::unique_ptr<ix::WebSocket> websocket_;
    std::atomic<bool> websocket_connected_ = false;
    std::mutex socket_events_mutex_;
    std::deque<SocketEvent> socket_events_;

    bool PerformAuth(bool is_register);
    void ConnectWebSocket();
    void DrainSocketEvents();
    void UpdateFilteredContacts();
    void SendMessage();
    void RefreshContacts();
};

} // namespace cim
