#include "ui/chat/ChatUI.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <utility>

namespace cim {

namespace {

std::string Trim(std::string value) {
    auto is_not_space = [](unsigned char character) {
        return !std::isspace(character);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
    return value;
}

std::filesystem::path Utf8Path(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()),
        reinterpret_cast<const char8_t*>(value.data() + value.size())));
}

} // namespace

void ChatUI::OpenSettings() {
    settings_return_tab_index_ = active_tab_index_;
    settings_host_ = client_config_.host;
    settings_port_ = std::to_string(client_config_.port);
    auto ca_source = std::find(
        settings_ca_sources_.begin(), settings_ca_sources_.end(), client_config_.ca_source);
    settings_ca_source_index_ = ca_source == settings_ca_sources_.end()
        ? 0
        : static_cast<int>(std::distance(settings_ca_sources_.begin(), ca_source));
    settings_custom_ca_path_ = client_config_.custom_ca_path;
    settings_error_.clear();
    active_tab_index_ = 2;
    settings_host_input_->TakeFocus();
}

void ChatUI::CloseSettings() {
    settings_error_.clear();
    active_tab_index_ = settings_return_tab_index_;
    if (active_tab_index_ == 1) {
        MarkSelectedContactRead();
        split_container_->TakeFocus();
    } else {
        login_container_->TakeFocus();
    }
}

void ChatUI::SaveSettings() {
    std::string host = Trim(settings_host_);
    if (host.empty()) {
        settings_error_ = "Server address cannot be empty";
        return;
    }
    if (host.find("://") != std::string::npos || host.find('/') != std::string::npos ||
        std::any_of(host.begin(), host.end(), [](unsigned char character) {
            return std::isspace(character);
        })) {
        settings_error_ = "Enter an IP address or hostname, not a URL";
        return;
    }

    unsigned int port = 0;
    const char* begin = settings_port_.data();
    const char* end = begin + settings_port_.size();
    auto [position, error] = std::from_chars(begin, end, port);
    if (error != std::errc{} || position != end || port == 0 || port > 65535) {
        settings_error_ = "Port must be a number between 1 and 65535";
        return;
    }

    if (settings_ca_source_index_ < 0 ||
        settings_ca_source_index_ >= static_cast<int>(settings_ca_sources_.size())) {
        settings_error_ = "Select a certificate authority source";
        return;
    }
    const std::string ca_source = settings_ca_sources_[settings_ca_source_index_];
    std::string custom_ca_path = Trim(settings_custom_ca_path_);
    if (ca_source == "CUSTOM") {
        if (custom_ca_path.empty() || custom_ca_path == "NONE" || custom_ca_path == "SYSTEM") {
            settings_error_ = "Enter a custom CA certificate file path";
            return;
        }
        std::error_code path_error;
        auto absolute_path = std::filesystem::absolute(
            Utf8Path(custom_ca_path), path_error);
        if (path_error || !std::filesystem::is_regular_file(absolute_path, path_error) || path_error) {
            settings_error_ = "Custom CA certificate is not a readable file";
            return;
        }
        const auto utf8_path = absolute_path.u8string();
        custom_ca_path.assign(
            reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size());
    }

    ClientConfig updated;
    updated.host = host;
    updated.port = static_cast<uint16_t>(port);
    updated.ca_source = ca_source;
    updated.custom_ca_path = custom_ca_path;
    const bool same_ca = updated.ca_source == client_config_.ca_source &&
        (updated.ca_source != "CUSTOM" ||
         updated.custom_ca_path == client_config_.custom_ca_path);
    if (updated.host == client_config_.host && updated.port == client_config_.port &&
        same_ca) {
        updated.username = client_config_.username;
        updated.session_token = client_config_.session_token;
    }
    if (!updated.Save(settings_error_)) {
        return;
    }
    client_config_ = std::move(updated);
    ReconnectWebSocket();
}

void ChatUI::ReconnectWebSocket() {
    connection_.Stop();

    auth_token_ = client_config_.session_token;
    pending_registration_username_.clear();
    pending_registration_watch_token_.clear();
    registration_request_username_.clear();
    registration_request_watch_token_.clear();
    registration_request_in_flight_ = false;
    auth_username_ = client_config_.username;
    auth_password_.clear();
    my_user_id_ = -1;
    my_name_.clear();
    contacts_.clear();
    filtered_indices_.clear();
    filtered_names_.clear();
    selected_contact_index_ = 0;
    notification_.clear();
    auth_error_.clear();
    auth_notice_ = auth_token_.empty() ? "" : "Restoring saved session...";
    auth_state_ = AuthState::Login;
    auth_action_label_ = "SIGN IN";
    auth_switch_label_ = "New here? Create an account";
    active_tab_index_ = 0;

    ConnectWebSocket();
    login_container_->TakeFocus();
}

void ChatUI::ConnectWebSocket() {
    connection_.Start(
        client_config_.WebSocketUrl(),
        trusted_ca_override_.empty()
            ? client_config_.TrustedCaData()
            : trusted_ca_override_);
}

} // namespace cim
