#include "ui/chat/ChatUI.hpp"
#include <nlohmann/json.hpp>
#include <sodium.h>

namespace cim {

namespace {

std::string GenerateWatchToken() {
    static constexpr char hex[] = "0123456789abcdef";
    unsigned char bytes[32];
    randombytes_buf(bytes, sizeof(bytes));
    std::string token;
    token.reserve(64);
    for (unsigned char byte : bytes) {
        token.push_back(hex[byte >> 4U]);
        token.push_back(hex[byte & 0x0fU]);
    }
    return token;
}

} // namespace

bool ChatUI::PerformAuth(bool is_register) {
    auth_notice_.clear();
    if (auth_username_.empty() || auth_password_.empty()) {
        auth_error_ = "Username and password cannot be empty";
        return false;
    }

    if (!connection_.IsConnected()) {
        auth_error_ = "Server is not connected";
        return false;
    }
    if (!auth_token_.empty() && auth_state_ != AuthState::LoggedIn) {
        auth_error_ = "Restoring the saved session...";
        return false;
    }
    if (is_register &&
        (registration_request_in_flight_ || !registration_request_username_.empty())) {
        auth_error_ = "A registration request is already awaiting a response";
        return false;
    }
    if (is_register && !pending_registration_username_.empty()) {
        if (!connection_.Send(nlohmann::json{
                {"type", "registration_watch"},
                {"username", pending_registration_username_},
                {"watch_token", pending_registration_watch_token_},
            }.dump())) {
            auth_error_ = "Failed to check the existing registration request";
            return false;
        }
        auth_error_.clear();
        auth_notice_ = "Checking the existing registration request...";
        return true;
    }

    nlohmann::json request = {
        {"type", is_register ? "register" : "login"},
        {"username", auth_username_},
        {"password", auth_password_},
    };
    if (is_register) {
        registration_request_username_ = auth_username_;
        registration_request_watch_token_ = GenerateWatchToken();
        request["watch_token"] = registration_request_watch_token_;
        registration_request_in_flight_ = true;
    }
    if (!connection_.Send(request.dump())) {
        if (is_register) {
            registration_request_username_.clear();
            registration_request_watch_token_.clear();
            registration_request_in_flight_ = false;
        }
        auth_error_ = "Failed to send authentication request";
        return false;
    }
    auth_error_.clear();
    return true;
}

void ChatUI::SignOut() {
    if (!connection_.IsConnected() || !connection_.IsAuthenticated()) {
        ResetAuthentication("Signed out locally; the remote session will expire automatically", false);
        return;
    }
    if (!connection_.Send(nlohmann::json{{"type", "logout"}}.dump())) {
        notification_ = "Failed to send sign-out request";
    }
}

void ChatUI::ResetAuthentication(const std::string& message, bool is_error) {
    if (!my_name_.empty()) {
        auth_username_ = my_name_;
    }
    auth_token_.clear();
    pending_registration_username_.clear();
    pending_registration_watch_token_.clear();
    registration_request_username_.clear();
    registration_request_watch_token_.clear();
    registration_request_in_flight_ = false;
    connection_.SetAuthenticated(false);
    my_user_id_ = -1;
    my_name_.clear();
    contacts_.clear();
    filtered_indices_.clear();
    filtered_names_.clear();
    selected_contact_index_ = 0;
    editing_name_ = false;
    search_query_.clear();
    chat_input_text_.clear();
    notification_.clear();
    auth_password_ = client_config_.remember_password
        ? client_config_.password
        : "";
    remember_password_ = client_config_.remember_password;
    auth_state_ = AuthState::Login;
    auth_action_label_ = "SIGN IN";
    auth_switch_label_ = "New here? Create an account";
    active_tab_index_ = 0;

    client_config_.username = auth_username_;
    std::string config_error;
    if (!client_config_.Save(config_error)) {
        auth_error_ = "Unable to save login settings: " + config_error;
        auth_notice_.clear();
    } else if (is_error) {
        auth_error_ = message;
        auth_notice_.clear();
    } else {
        auth_error_.clear();
        auth_notice_ = message;
    }
    login_password_input_->TakeFocus();
}

} // namespace cim
