#include "ChatUI.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/component.hpp"
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>

namespace cim {

namespace {

std::string FormatTimestamp(int64_t timestamp) {
    if (timestamp <= 0) {
        return "Unknown time";
    }
    std::time_t time = static_cast<std::time_t>(timestamp);
    std::tm local_time{};
#ifdef _WIN32
    if (localtime_s(&local_time, &time) != 0) {
        return "Unknown time";
    }
#else
    if (localtime_r(&time, &local_time) == nullptr) {
        return "Unknown time";
    }
#endif

    std::ostringstream output;
    output << std::put_time(&local_time, "%Y-%m-%d %H:%M");
    return output.str();
}

std::string Trim(std::string value) {
    auto is_not_space = [](unsigned char character) {
        return !std::isspace(character);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
    return value;
}

bool EqualUsernames(const std::string& left, const std::string& right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
}

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

ChatUI::ChatUI(ftxui::Closure request_refresh)
    : request_refresh_(std::move(request_refresh)) {
    ix::initNetSystem();
    websocket_ = std::make_unique<ix::WebSocket>();
    client_config_ = ClientConfig::Load();
    auth_username_ = client_config_.username;
    auth_token_ = client_config_.session_token;
    if (!auth_token_.empty()) {
        auth_notice_ = "Restoring saved session...";
    }
    settings_host_ = client_config_.host;
    settings_port_ = std::to_string(client_config_.port);

    ftxui::InputOption user_option;
    user_option.multiline = false;
    user_option.transform = [](ftxui::InputState state) {
        if (state.focused) {
            return state.element | ftxui::color(ftxui::Color::Cyan);
        }
        if (state.is_placeholder) {
            return state.element | ftxui::color(ftxui::Color::GrayDark);
        }
        return state.element;
    };
    login_username_input_ = ftxui::Input(&auth_username_, "Username", user_option);

    ftxui::InputOption pass_option;
    pass_option.multiline = false;
    pass_option.password = true;
    pass_option.transform = user_option.transform;
    login_password_input_ = ftxui::Input(&auth_password_, "Password", pass_option);

    auto primary_button = ftxui::ButtonOption::Simple();
    primary_button.transform = [](const ftxui::EntryState& state) {
        auto label = ftxui::text(state.label) | ftxui::bold;
        auto element = ftxui::hbox({
            ftxui::filler(),
            label,
            ftxui::filler(),
        });
        if (state.focused) {
            element = element |
                ftxui::color(ftxui::Color::Black) |
                ftxui::bgcolor(ftxui::Color::Cyan);
        } else {
            element = element | ftxui::color(ftxui::Color::Cyan);
        }
        return element | ftxui::borderRounded | ftxui::flex;
    };
    login_btn_ = ftxui::Button(
        &auth_action_label_,
        [this] { PerformAuth(auth_state_ == AuthState::Register); },
        primary_button
    );

    auto secondary_button = ftxui::ButtonOption::Simple();
    secondary_button.transform = [](const ftxui::EntryState& state) {
        auto element = ftxui::text(state.label) | ftxui::center;
        if (state.focused) {
            return element | ftxui::bold | ftxui::color(ftxui::Color::Cyan);
        }
        return element | ftxui::color(ftxui::Color::GrayDark);
    };
    switch_btn_ = ftxui::Button(&auth_switch_label_, [this] {
        auth_state_ = (auth_state_ == AuthState::Login) ? AuthState::Register : AuthState::Login;
        const bool is_login = auth_state_ == AuthState::Login;
        auth_action_label_ = is_login ? "SIGN IN" : "CREATE ACCOUNT";
        auth_switch_label_ = is_login
            ? "New here? Create an account"
            : "Already registered? Sign in";
        auth_error_.clear();
        auth_notice_.clear();
    }, secondary_button);

    login_settings_btn_ = ftxui::Button("Server settings", [this] {
        OpenSettings();
    }, secondary_button);

    chat_settings_btn_ = ftxui::Button("Settings", [this] {
        OpenSettings();
    }, secondary_button);
    sign_out_btn_ = ftxui::Button("SIGN OUT", [this] {
        SignOut();
    }, secondary_button);

    ftxui::InputOption settings_input_option;
    settings_input_option.multiline = false;
    settings_input_option.transform = user_option.transform;
    settings_host_input_ = ftxui::Input(
        &settings_host_, "127.0.0.1 or server.example.com", settings_input_option);
    settings_port_input_ = ftxui::Input(&settings_port_, "9001", settings_input_option);

    settings_save_btn_ = ftxui::Button("SAVE & RECONNECT", [this] {
        SaveSettings();
    }, primary_button);
    settings_back_btn_ = ftxui::Button("BACK", [this] {
        CloseSettings();
    }, secondary_button);

    login_container_ = ftxui::Container::Vertical({
        login_username_input_,
        login_password_input_,
        login_btn_,
        switch_btn_,
        login_settings_btn_,
    });

    settings_container_ = ftxui::Container::Vertical({
        settings_host_input_,
        settings_port_input_,
        settings_save_btn_,
        settings_back_btn_,
    });

    ftxui::InputOption name_option;
    name_option.multiline = false;
    name_option.on_enter = [this] {
        name_input_text_.erase(std::remove(name_input_text_.begin(), name_input_text_.end(), '\n'), name_input_text_.end());
        editing_name_ = false;
        name_button_->TakeFocus();
    };
    my_name_input_ = ftxui::Input(&name_input_text_, "My Name", name_option);

    ftxui::InputOption search_option;
    search_option.multiline = false;
    search_option.on_change = [this] {
        UpdateFilteredContacts();
        MarkSelectedContactRead();
    };
    search_input_ = ftxui::Input(&search_query_, "Search contacts...", search_option);

    name_button_ = ftxui::Button(
        &my_name_,
        [this] {
            editing_name_ = true;
            name_input_text_ = my_name_;
            my_name_input_->TakeFocus();
        },
        [] {
            auto opt = ftxui::ButtonOption::Simple();
            opt.transform = [](const ftxui::EntryState& s) {
                auto element = ftxui::text(s.label);
                if (s.focused) {
                    element = element | ftxui::bold | ftxui::color(ftxui::Color::Cyan);
                } else {
                    element = element | ftxui::color(ftxui::Color::Green);
                }
                return element;
            };
            return opt;
        }()
    );

    ftxui::MenuOption menu_option;
    menu_option.entries_option.transform = [this](const ftxui::EntryState& s) {
        auto element = ftxui::text(" " + s.label + " ");
        bool unread = false;
        if (s.index >= 0 && s.index < static_cast<int>(filtered_indices_.size())) {
            const int contact_index = filtered_indices_[s.index];
            unread = contact_index >= 0 &&
                contact_index < static_cast<int>(contacts_.size()) &&
                contacts_[contact_index].unread;
        }
        if (s.active && s.focused) {
            element = element |
                ftxui::bold |
                ftxui::color(ftxui::Color::Black) |
                ftxui::bgcolor(ftxui::Color::Cyan);
        } else if (unread) {
            element = element | ftxui::bold | ftxui::color(ftxui::Color::MagentaLight);
        } else if (s.active) {
            element = element | ftxui::bold | ftxui::color(ftxui::Color::Cyan);
        } else {
            element = element | ftxui::color(ftxui::Color::White);
        }
        if (unread) {
            element = element | ftxui::blink;
        }
        return element;
    };
    menu_option.on_change = [this] {
        MarkSelectedContactRead();
    };

    contact_menu_ = ftxui::Menu(&filtered_names_, &selected_contact_index_, menu_option);

    ftxui::InputOption chat_option;
    chat_option.multiline = false;
    chat_option.on_enter = [this] { SendMessage(); };
    chat_input_ = ftxui::Input(&chat_input_text_, "Type a message...", chat_option);

    left_container_ = ftxui::Container::Vertical({
        name_button_,
        sign_out_btn_,
        chat_settings_btn_,
        search_input_,
        contact_menu_,
    });

    right_container_ = ftxui::Container::Vertical({
        chat_input_,
    });

    split_container_ = ftxui::Container::Horizontal({
        left_container_,
        right_container_,
    }, &active_pane_);

    root_container_ = ftxui::Container::Tab({
        login_container_,
        split_container_,
        settings_container_,
    }, &active_tab_index_);

    ConnectWebSocket();
}

ChatUI::~ChatUI() {
    if (websocket_) {
        websocket_->stop();
        websocket_.reset();
    }
    ix::uninitNetSystem();
}

bool ChatUI::PerformAuth(bool is_register) {
    auth_notice_.clear();
    if (auth_username_.empty() || auth_password_.empty()) {
        auth_error_ = "Username and password cannot be empty";
        return false;
    }

    if (!websocket_connected_) {
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
        auto result = websocket_->send(nlohmann::json{
            {"type", "registration_watch"},
            {"username", pending_registration_username_},
            {"watch_token", pending_registration_watch_token_},
        }.dump());
        if (!result.success) {
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
    auto result = websocket_->send(request.dump());
    if (!result.success) {
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

void ChatUI::OpenSettings() {
    settings_return_tab_index_ = active_tab_index_;
    settings_host_ = client_config_.host;
    settings_port_ = std::to_string(client_config_.port);
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

    ClientConfig updated{host, static_cast<uint16_t>(port)};
    if (updated.host == client_config_.host && updated.port == client_config_.port) {
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
    if (websocket_) {
        websocket_->stop();
        websocket_.reset();
    }
    {
        std::lock_guard lock(socket_events_mutex_);
        socket_events_.clear();
    }

    websocket_connected_ = false;
    websocket_authenticated_ = false;
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

    websocket_ = std::make_unique<ix::WebSocket>();
    ConnectWebSocket();
    login_container_->TakeFocus();
}

void ChatUI::ConnectWebSocket() {
    websocket_->setUrl(client_config_.WebSocketUrl());
    websocket_->setPingInterval(30);
    websocket_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
        SocketEvent event;
        switch (message->type) {
            case ix::WebSocketMessageType::Open:
                websocket_connected_ = true;
                websocket_authenticated_ = false;
                event.type = SocketEventType::Connected;
                break;
            case ix::WebSocketMessageType::Close:
                websocket_connected_ = false;
                websocket_authenticated_ = false;
                event.type = SocketEventType::Disconnected;
                event.content = message->closeInfo.reason;
                break;
            case ix::WebSocketMessageType::Error:
                websocket_connected_ = false;
                websocket_authenticated_ = false;
                event.type = SocketEventType::Error;
                event.content = message->errorInfo.reason;
                break;
            case ix::WebSocketMessageType::Message:
                event.type = SocketEventType::Message;
                event.content = message->str;
                break;
            default:
                return;
        }

        {
            std::lock_guard lock(socket_events_mutex_);
            socket_events_.push_back(std::move(event));
        }
        request_refresh_();
    });
    websocket_->start();
}

void ChatUI::DrainSocketEvents() {
    std::deque<SocketEvent> events;
    {
        std::lock_guard lock(socket_events_mutex_);
        events.swap(socket_events_);
    }

    for (auto& event : events) {
        if (event.type == SocketEventType::Connected) {
            if (!auth_token_.empty()) {
                auto result = websocket_->send(nlohmann::json{
                    {"type", "resume"},
                    {"token", auth_token_},
                }.dump());
                if (!result.success) {
                    auth_token_.clear();
                    auth_notice_.clear();
                    auth_error_ = "Failed to restore the saved session";
                }
            } else if (!pending_registration_username_.empty()) {
                websocket_->send(nlohmann::json{
                    {"type", "registration_watch"},
                    {"username", pending_registration_username_},
                    {"watch_token", pending_registration_watch_token_},
                }.dump());
            } else if (!registration_request_username_.empty()) {
                websocket_->send(nlohmann::json{
                    {"type", "registration_watch"},
                    {"username", registration_request_username_},
                    {"watch_token", registration_request_watch_token_},
                }.dump());
            } else {
                auth_error_.clear();
            }
            continue;
        }
        if (event.type == SocketEventType::Disconnected) {
            registration_request_in_flight_ = false;
            if (auth_state_ == AuthState::LoggedIn) {
                notification_ = "Disconnected; reconnecting...";
            } else {
                auth_error_ = "Disconnected; reconnecting...";
            }
            continue;
        }
        if (event.type == SocketEventType::Error) {
            registration_request_in_flight_ = false;
            if (auth_state_ == AuthState::LoggedIn) {
                notification_ = "WebSocket: " + event.content;
            } else {
                auth_error_ = "WebSocket: " + event.content;
            }
            continue;
        }

        auto payload = nlohmann::json::parse(event.content, nullptr, false);
        if (payload.is_discarded()) {
            notification_ = "Invalid WebSocket response";
            continue;
        }

        std::string type = payload.value("type", "");
        if (type == "session_invalid") {
            ResetAuthentication(
                payload.value("message", "Session expired; sign in again"), true);
            continue;
        }
        if (type == "logout_success") {
            ResetAuthentication("Signed out", false);
            continue;
        }
        if (type == "error") {
            std::string error = payload.value("error", "Server error");
            if (auth_state_ == AuthState::LoggedIn) {
                notification_ = error;
            } else {
                if (registration_request_in_flight_) {
                    registration_request_username_.clear();
                    registration_request_watch_token_.clear();
                    registration_request_in_flight_ = false;
                }
                auth_notice_.clear();
                auth_error_ = error;
            }
            continue;
        }
        if (type == "registration_pending") {
            const std::string username = payload.value(
                "username",
                pending_registration_username_.empty()
                    ? auth_username_
                    : pending_registration_username_);
            const std::string watch_token = payload.value("watch_token", "");
            const bool matches_pending =
                EqualUsernames(username, pending_registration_username_) &&
                watch_token == pending_registration_watch_token_;
            const bool matches_request =
                EqualUsernames(username, registration_request_username_) &&
                watch_token == registration_request_watch_token_;
            if (auth_state_ == AuthState::LoggedIn ||
                (!matches_pending && !matches_request)) {
                continue;
            }
            registration_request_in_flight_ = false;
            pending_registration_username_ = username;
            pending_registration_watch_token_ = watch_token;
            registration_request_username_.clear();
            registration_request_watch_token_.clear();
            auth_username_ = username;
            auth_state_ = AuthState::Login;
            auth_action_label_ = "SIGN IN";
            auth_switch_label_ = "New here? Create an account";
            auth_password_.clear();
            auth_error_.clear();
            auth_notice_ = payload.value(
                "message", "Registration submitted; wait for administrator approval, then sign in");
            login_username_input_->TakeFocus();
            continue;
        }
        if (type == "registration_approved") {
            const std::string username = payload.value("username", "");
            const std::string watch_token = payload.value("watch_token", "");
            const bool matches_pending =
                EqualUsernames(username, pending_registration_username_) &&
                watch_token == pending_registration_watch_token_;
            const bool matches_request =
                EqualUsernames(username, registration_request_username_) &&
                watch_token == registration_request_watch_token_;
            if (auth_state_ == AuthState::LoggedIn ||
                (!matches_pending && !matches_request)) {
                continue;
            }
            pending_registration_username_.clear();
            pending_registration_watch_token_.clear();
            registration_request_username_.clear();
            registration_request_watch_token_.clear();
            registration_request_in_flight_ = false;
            auth_state_ = AuthState::Login;
            auth_action_label_ = "SIGN IN";
            auth_switch_label_ = "New here? Create an account";
            auth_password_.clear();
            auth_error_.clear();
            auth_notice_ = payload.value(
                "message", "Registration approved. Sign in to continue");
            login_password_input_->TakeFocus();
            continue;
        }
        if (type == "registration_rejected") {
            const std::string username = payload.value("username", "");
            const std::string watch_token = payload.value("watch_token", "");
            const bool matches_pending =
                EqualUsernames(username, pending_registration_username_) &&
                watch_token == pending_registration_watch_token_;
            const bool matches_request =
                EqualUsernames(username, registration_request_username_) &&
                watch_token == registration_request_watch_token_;
            if (auth_state_ == AuthState::LoggedIn ||
                (!matches_pending && !matches_request)) {
                continue;
            }
            pending_registration_username_.clear();
            pending_registration_watch_token_.clear();
            registration_request_username_.clear();
            registration_request_watch_token_.clear();
            registration_request_in_flight_ = false;
            auth_state_ = AuthState::Register;
            auth_action_label_ = "CREATE ACCOUNT";
            auth_switch_label_ = "Already registered? Sign in";
            auth_password_.clear();
            auth_notice_.clear();
            const std::string reason = payload.value("reason", "");
            auth_error_ = reason.empty()
                ? payload.value("message", "Registration rejected; you may submit a new request")
                : "Registration rejected: " + reason;
            login_password_input_->TakeFocus();
            continue;
        }
        if (type == "auth_success") {
            auth_token_ = payload.value("token", "");
            pending_registration_username_.clear();
            pending_registration_watch_token_.clear();
            registration_request_username_.clear();
            registration_request_watch_token_.clear();
            registration_request_in_flight_ = false;
            my_user_id_ = payload.value("user_id", int64_t{-1});
            my_name_ = payload.value("username", auth_username_);
            auth_username_ = my_name_;
            auth_password_.clear();
            auth_state_ = AuthState::LoggedIn;
            websocket_authenticated_ = true;
            active_tab_index_ = 1;
            auth_error_.clear();
            auth_notice_.clear();
            client_config_.username = my_name_;
            client_config_.session_token = auth_token_;
            std::string config_error;
            notification_ = client_config_.Save(config_error)
                ? ""
                : "Logged in, but the session could not be saved: " + config_error;
            websocket_->send(nlohmann::json{{"type", "users"}}.dump());
            split_container_->TakeFocus();
            continue;
        }
        if (type == "users") {
            if (auth_state_ != AuthState::LoggedIn || !websocket_authenticated_) {
                continue;
            }
            for (auto& contact : contacts_) {
                contact.online = false;
            }
            for (const auto& user : payload.value("users", nlohmann::json::array())) {
                int64_t user_id = user.value("id", int64_t{-1});
                std::string username = user.value("username", "");
                if (user_id == my_user_id_ || username.empty()) {
                    continue;
                }
                auto contact = std::find_if(contacts_.begin(), contacts_.end(), [user_id](const Contact& item) {
                    return item.id == user_id;
                });
                if (contact == contacts_.end()) {
                    contacts_.push_back({user_id, username, user.value("online", false), false, {}});
                } else {
                    contact->name = username;
                    contact->online = user.value("online", false);
                }
            }
            continue;
        }
        if (type == "chat") {
            if (auth_state_ != AuthState::LoggedIn || !websocket_authenticated_) {
                continue;
            }
            int64_t sender_id = payload.value("sender_id", int64_t{-1});
            int64_t recipient_id = payload.value("recipient_id", int64_t{-1});
            if (sender_id != my_user_id_ && recipient_id != my_user_id_) {
                continue;
            }
            const bool is_me = sender_id == my_user_id_;
            const int64_t contact_id = is_me ? recipient_id : sender_id;
            std::string contact_name = is_me
                ? payload.value("recipient_name", "")
                : payload.value("sender_name", "");
            auto contact = std::find_if(contacts_.begin(), contacts_.end(), [contact_id](const Contact& item) {
                return item.id == contact_id;
            });
            if (contact == contacts_.end()) {
                contacts_.push_back({contact_id, contact_name, true, false, {}});
                contact = std::prev(contacts_.end());
            }
            const bool selected_contact_is_visible =
                active_tab_index_ == 1 &&
                selected_contact_index_ >= 0 &&
                selected_contact_index_ < static_cast<int>(filtered_indices_.size()) &&
                contacts_[filtered_indices_[selected_contact_index_]].id == contact_id;
            if (!is_me && !selected_contact_is_visible) {
                contact->unread = true;
            }
            contact->messages.push_back({
                is_me,
                payload.value("content", ""),
                payload.value("sent_at", int64_t{0}),
            });
        }
    }
    UpdateFilteredContacts();
    MarkSelectedContactRead();
}

void ChatUI::UpdateFilteredContacts() {
    filtered_indices_.clear();
    filtered_names_.clear();
    for (size_t i = 0; i < contacts_.size(); ++i) {
        std::string display_name = contacts_[i].name;
        if (contacts_[i].unread) {
            display_name += " [NEW]";
        }
        display_name += (contacts_[i].online ? " [Online]" : " [Offline]");
        if (search_query_.empty() || contacts_[i].name.find(search_query_) != std::string::npos) {
            filtered_indices_.push_back(i);
            filtered_names_.push_back(display_name);
        }
    }
    if (filtered_names_.empty()) {
        selected_contact_index_ = -1;
    } else {
        selected_contact_index_ = std::clamp(
            selected_contact_index_, 0, static_cast<int>(filtered_names_.size()) - 1);
    }
}

void ChatUI::MarkSelectedContactRead() {
    if (active_tab_index_ != 1 ||
        selected_contact_index_ < 0 ||
        selected_contact_index_ >= static_cast<int>(filtered_indices_.size())) {
        return;
    }

    const int contact_index = filtered_indices_[selected_contact_index_];
    if (!contacts_[contact_index].unread) {
        return;
    }
    contacts_[contact_index].unread = false;
    UpdateFilteredContacts();
}

void ChatUI::SendMessage() {
    UpdateFilteredContacts();
    if (selected_contact_index_ >= 0 && selected_contact_index_ < (int)filtered_indices_.size()) {
        int actual_idx = filtered_indices_[selected_contact_index_];
        chat_input_text_.erase(std::remove(chat_input_text_.begin(), chat_input_text_.end(), '\n'), chat_input_text_.end());
        if (chat_input_text_.empty()) {
            notification_ = "Warning: Cannot send empty message!";
        } else if (!websocket_authenticated_) {
            notification_ = "WebSocket is not authenticated";
        } else {
            nlohmann::json message = {
                {"type", "chat"},
                {"recipient_id", contacts_[actual_idx].id},
                {"content", chat_input_text_},
            };
            auto result = websocket_->send(message.dump());
            if (result.success) {
                notification_.clear();
                chat_input_text_.clear();
            } else {
                notification_ = "Failed to send WebSocket message";
            }
        }
    }
}

void ChatUI::SignOut() {
    if (!websocket_connected_ || !websocket_authenticated_) {
        ResetAuthentication("Signed out locally; the remote session will expire automatically", false);
        return;
    }
    if (!websocket_->send(nlohmann::json{{"type", "logout"}}.dump()).success) {
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
    websocket_authenticated_ = false;
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
    auth_password_.clear();
    auth_state_ = AuthState::Login;
    auth_action_label_ = "SIGN IN";
    auth_switch_label_ = "New here? Create an account";
    active_tab_index_ = 0;

    client_config_.username = auth_username_;
    client_config_.session_token.clear();
    std::string config_error;
    if (!client_config_.Save(config_error)) {
        auth_error_ = "Unable to clear the saved session: " + config_error;
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

ftxui::Component ChatUI::GetComponent() {
    return ftxui::Renderer(root_container_, [this] {
        using namespace ftxui;

        if (active_tab_index_ == 2) {
            auto host_field = vbox(Elements{
                text("SERVER ADDRESS") | color(Color::GrayDark),
                hbox(Elements{
                    text(" > ") | bold | color(Color::Cyan),
                    settings_host_input_->Render() | flex,
                }) | borderLight,
            });
            auto port_field = vbox(Elements{
                text("PORT") | color(Color::GrayDark),
                hbox(Elements{
                    text(" > ") | bold | color(Color::Cyan),
                    settings_port_input_->Render() | flex,
                }) | borderLight,
            });
            auto status = websocket_connected_
                ? text("CONNECTED") | bold | color(Color::Green)
                : text("CONNECTING...") | color(Color::Yellow);
            auto error = settings_error_.empty()
                ? text(" ")
                : text("! " + settings_error_) | bold | color(Color::RedLight);

            auto card = vbox(Elements{
                text("SERVER SETTINGS") | bold | color(Color::Cyan) | center,
                text("Configure where cim connects") | color(Color::GrayDark) | center,
                separator(),
                hbox(Elements{
                    text(" Current: ") | color(Color::GrayDark),
                    text(client_config_.WebSocketUrl()),
                    filler(),
                    status,
                    text(" "),
                }),
                text(" "),
                host_field,
                text(" "),
                port_field,
                error | center,
                settings_save_btn_->Render() | size(WIDTH, EQUAL, 48) | hcenter,
                text(" "),
                settings_back_btn_->Render() | center,
            }) | size(WIDTH, EQUAL, 56) | borderRounded;

            return card | center;
        }

        if (auth_state_ != AuthState::LoggedIn) {
            const bool is_login = auth_state_ == AuthState::Login;
            auto username_field = vbox(Elements{
                text("USERNAME") | color(Color::GrayDark),
                hbox(Elements{
                    text(" > ") | bold | color(Color::Cyan),
                    login_username_input_->Render() | flex,
                }) | borderLight,
            });
            auto password_field = vbox(Elements{
                text("PASSWORD") | color(Color::GrayDark),
                hbox(Elements{
                    text(" > ") | bold | color(Color::Cyan),
                    login_password_input_->Render() | flex,
                }) | borderLight,
            });
            Element status = text(" ");
            if (!auth_error_.empty()) {
                status = text("! " + auth_error_) | bold | color(Color::RedLight);
            } else if (!auth_notice_.empty()) {
                status = text(auth_notice_) | bold | color(Color::GreenLight);
            }

            auto card = vbox(Elements{
                text("cim") | bold | color(Color::Cyan) | center,
                text("COMMAND INSTANT MESSENGER") | color(Color::GrayDark) | center,
                text(" "),
                text(is_login ? " SIGN IN " : " CREATE ACCOUNT ") |
                    bold |
                    color(Color::Black) |
                    bgcolor(Color::Cyan) |
                    center,
                text(" "),
                username_field,
                text(" "),
                password_field,
                status | center,
                login_btn_->Render() | size(WIDTH, EQUAL, 44) | hcenter,
                text(" "),
                switch_btn_->Render() | center,
                login_settings_btn_->Render() | center,
            }) | size(WIDTH, EQUAL, 50) | borderRounded;

            return card | center;
        }

        if (selected_contact_index_ >= (int)filtered_names_.size()) {
            selected_contact_index_ = filtered_names_.empty() ? -1 : (int)filtered_names_.size() - 1;
        }

        Element name_display = editing_name_ ? my_name_input_->Render() : name_button_->Render();

        auto left_pane = vbox(Elements{
            hbox(Elements{
                text(" USER: ") | bold | color(Color::Cyan),
                editing_name_ ? my_name_input_->Render() : name_button_->Render(),
                filler(),
                sign_out_btn_->Render(),
                text(" "),
                chat_settings_btn_->Render(),
            }),
            separator(),
            hbox(Elements{
                text(" FILTER: ") | dim,
                search_input_->Render(),
            }),
            separator(),
            contact_menu_->Render() | vscroll_indicator | frame | yflex,
        }) | size(WIDTH, EQUAL, 35);

        Element right_pane;
        if (selected_contact_index_ < 0 || selected_contact_index_ >= (int)filtered_indices_.size()) {
            right_pane = vbox(Elements{
                filler(),
                text(">> Select a contact from the left to start chatting <<") | dim | center,
                filler(),
            }) | xflex;
        } else {
            int idx = filtered_indices_[selected_contact_index_];
            const auto& contact = contacts_[idx];

            Elements msg_elements;
            if (contact.messages.empty()) {
                msg_elements.push_back(text("No messages yet with " + contact.name) | dim | italic);
            } else {
                for (const auto& message : contact.messages) {
                    auto timestamp = text(FormatTimestamp(message.sent_at)) |
                        color(Color::GrayDark);
                    if (message.is_me) {
                        msg_elements.push_back(vbox(Elements{
                            hbox(Elements{filler(), timestamp}),
                            hbox(Elements{
                                filler(),
                                text(my_name_ + ": " + message.content) | color(Color::Green),
                            }),
                        }));
                    } else {
                        msg_elements.push_back(vbox(Elements{
                            hbox(Elements{timestamp, filler()}),
                            hbox(Elements{
                                text(contact.name + ": " + message.content) | color(Color::Cyan),
                                filler(),
                            }),
                        }));
                    }
                }
            }

            auto chat_history = vbox(msg_elements) | vscroll_indicator | frame | yflex;

            Elements footer = {
                hbox(Elements{
                    text(" > ") | bold | color(Color::Yellow),
                    chat_input_->Render(),
                })
            };
            if (!notification_.empty()) {
                footer.push_back(text(" " + notification_) | color(Color::Red) | bold);
            }

            right_pane = vbox(Elements{
                text(" CHAT: " + contact.name) | bold | color(Color::Yellow) | center,
                separator(),
                chat_history,
                separator(),
                vbox(footer),
            }) | xflex;
        }

        return hbox(Elements{
            left_pane,
            separator(),
            right_pane,
        });
    }) | ftxui::CatchEvent([this](ftxui::Event event) {
        if (event == ftxui::Event::Custom) {
            DrainSocketEvents();
            return true;
        }
        if (event == ftxui::Event::Escape) {
            if (active_tab_index_ == 2) {
                CloseSettings();
                return true;
            }
            if (auth_state_ == AuthState::LoggedIn && editing_name_) {
                name_input_text_ = my_name_;
                editing_name_ = false;
                name_button_->TakeFocus();
                return true;
            }
            if (auth_state_ == AuthState::LoggedIn && search_input_->Focused()) {
                active_pane_ = 0;
                contact_menu_->TakeFocus();
                return true;
            }
        }
        if (active_tab_index_ == 2) return false;
        if (auth_state_ != AuthState::LoggedIn) return false;
        if (editing_name_ ) {
            return false;
        }

        if (event == ftxui::Event::ArrowLeft) {
            active_pane_ = 0;
            return true;
        }
        if (event == ftxui::Event::ArrowRight) {
            active_pane_ = 1;
            return true;
        }

        if (active_pane_ == 0 && contact_menu_->Focused() && selected_contact_index_ == 0 && event == ftxui::Event::ArrowUp) {
            search_input_->TakeFocus();
            return true;
        }

        return false;
    });
}

} // namespace cim
