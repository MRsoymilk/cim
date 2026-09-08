#include "ui/chat/ChatUI.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <iterator>

namespace cim {

namespace {

bool EqualUsernames(const std::string& left, const std::string& right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
}

} // namespace

void ChatUI::DrainSocketEvents() {
    auto events = connection_.DrainEvents();

    for (auto& event : events) {
        if (event.type == ClientConnection::EventType::Connected) {
            if (!auth_token_.empty()) {
                if (!connection_.Send(nlohmann::json{
                        {"type", "resume"},
                        {"token", auth_token_},
                    }.dump())) {
                    auth_token_.clear();
                    auth_notice_.clear();
                    auth_error_ = "Failed to restore the saved session";
                }
            } else if (!pending_registration_username_.empty()) {
                connection_.Send(nlohmann::json{
                    {"type", "registration_watch"},
                    {"username", pending_registration_username_},
                    {"watch_token", pending_registration_watch_token_},
                }.dump());
            } else if (!registration_request_username_.empty()) {
                connection_.Send(nlohmann::json{
                    {"type", "registration_watch"},
                    {"username", registration_request_username_},
                    {"watch_token", registration_request_watch_token_},
                }.dump());
            } else {
                auth_error_.clear();
            }
            continue;
        }
        if (event.type == ClientConnection::EventType::Disconnected) {
            registration_request_in_flight_ = false;
            if (auth_state_ == AuthState::LoggedIn) {
                notification_ = "Disconnected; reconnecting...";
            } else {
                auth_error_ = "Disconnected; reconnecting...";
            }
            continue;
        }
        if (event.type == ClientConnection::EventType::Error) {
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
            connection_.SetAuthenticated(true);
            active_tab_index_ = 1;
            auth_error_.clear();
            auth_notice_.clear();
            client_config_.username = my_name_;
            client_config_.session_token = auth_token_;
            std::string config_error;
            notification_ = client_config_.Save(config_error)
                ? ""
                : "Logged in, but the session could not be saved: " + config_error;
            connection_.Send(nlohmann::json{{"type", "users"}}.dump());
            split_container_->TakeFocus();
            continue;
        }
        if (type == "users") {
            if (auth_state_ != AuthState::LoggedIn || !connection_.IsAuthenticated()) {
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
            if (auth_state_ != AuthState::LoggedIn || !connection_.IsAuthenticated()) {
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

} // namespace cim
