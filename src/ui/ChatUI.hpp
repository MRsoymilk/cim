#pragma once

#include "ftxui/component/component.hpp"
#include <string>
#include <vector>
#include <thread>
#include <cstdint>

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
    ChatUI();
    ~ChatUI();
    ftxui::Component GetComponent();

private:
    AuthState auth_state_ = AuthState::Login;
    std::string auth_username_ = "";
    std::string auth_password_ = "";
    std::string auth_error_ = "";
    std::string auth_token_ = "";
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

    bool PerformAuth(bool is_register);
    void UpdateFilteredContacts();
    void SendMessage();
    void RefreshContacts();
};

} // namespace cim
