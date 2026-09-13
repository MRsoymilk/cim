#pragma once

#include "config/ClientConfig.hpp"
#include "net/ClientConnection.hpp"
#include "ui/chat/ChatTypes.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/task.hpp"
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <cstdint>

namespace cim {

class ChatUI {
public:
    explicit ChatUI(
        ftxui::Closure request_refresh, std::string trusted_ca_override = {});
    ~ChatUI();
    ftxui::Component GetComponent();

private:
    ClientConnection connection_;
    std::atomic_bool animate_auth_status_ = false;
    std::jthread render_timer_;
    AuthState auth_state_ = AuthState::Login;
    std::string auth_username_ = "";
    std::string auth_password_ = "";
    bool remember_password_ = false;
    std::string auth_error_ = "";
    std::string auth_notice_ = "";
    std::string auth_token_ = "";
    std::string pending_registration_username_ = "";
    std::string pending_registration_watch_token_ = "";
    std::string registration_request_username_ = "";
    std::string registration_request_watch_token_ = "";
    bool registration_request_in_flight_ = false;
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

    ClientConfig client_config_;
    std::string trusted_ca_override_;
    std::string settings_host_;
    std::string settings_port_;
    std::vector<std::string> settings_ca_sources_{"BUNDLED", "SYSTEM", "CUSTOM"};
    int settings_ca_source_index_ = 0;
    std::string settings_custom_ca_path_;
    std::string settings_error_;
    int settings_return_tab_index_ = 0;

    int active_pane_ = 0; // 0 = left, 1 = right
    int active_tab_index_ = 0; // 0 = login, 1 = chat, 2 = settings

    ftxui::Component login_username_input_;
    ftxui::Component login_password_input_;
    ftxui::Component remember_password_checkbox_;
    ftxui::Component login_btn_;
    ftxui::Component switch_btn_;
    ftxui::Component login_settings_btn_;
    ftxui::Component chat_settings_btn_;
    ftxui::Component sign_out_btn_;
    ftxui::Component settings_host_input_;
    ftxui::Component settings_port_input_;
    ftxui::Component settings_ca_source_toggle_;
    ftxui::Component settings_custom_ca_input_;
    ftxui::Component settings_save_btn_;
    ftxui::Component settings_back_btn_;
    ftxui::Component my_name_input_;
    ftxui::Component search_input_;
    ftxui::Component chat_input_;
    ftxui::Component name_button_;
    ftxui::Component contact_menu_;
    ftxui::Component left_container_;
    ftxui::Component right_container_;
    ftxui::Component split_container_;
    ftxui::Component login_container_;
    ftxui::Component settings_container_;
    ftxui::Component root_container_;

    std::vector<int> filtered_indices_;
    std::vector<std::string> filtered_names_;

    bool PerformAuth(bool is_register);
    void OpenSettings();
    void CloseSettings();
    void SaveSettings();
    void ReconnectWebSocket();
    void ConnectWebSocket();
    void DrainSocketEvents();
    void UpdateFilteredContacts();
    void MarkSelectedContactRead();
    void SendMessage();
    void SignOut();
    void ResetAuthentication(const std::string& message, bool is_error);
};

} // namespace cim
