#include "ChatUI.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/component.hpp"
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iterator>
#include <utility>

namespace cim {

ChatUI::ChatUI(ftxui::Closure request_refresh)
    : request_refresh_(std::move(request_refresh)) {
    ix::initNetSystem();
    websocket_ = std::make_unique<ix::WebSocket>();

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
    }, secondary_button);

    login_container_ = ftxui::Container::Vertical({
        login_username_input_,
        login_password_input_,
        login_btn_,
        switch_btn_,
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
                    element = element | ftxui::bold | ftxui::color(ftxui::Color::Yellow);
                } else {
                    element = element | ftxui::color(ftxui::Color::Green);
                }
                return element;
            };
            return opt;
        }()
    );

    ftxui::MenuOption menu_option;
    menu_option.entries_option.transform = [](const ftxui::EntryState& s) {
        auto element = ftxui::text(" " + s.label + " ");
        if (s.active) {
            element = element | ftxui::bold | ftxui::color(ftxui::Color::Yellow) | ftxui::inverted;
        } else if (s.focused) {
            element = element | ftxui::bold | ftxui::color(ftxui::Color::Cyan);
        } else {
            element = element | ftxui::color(ftxui::Color::White);
        }
        return element;
    };

    contact_menu_ = ftxui::Menu(&filtered_names_, &selected_contact_index_, menu_option);

    ftxui::InputOption chat_option;
    chat_option.multiline = false;
    chat_option.on_enter = [this] { SendMessage(); };
    chat_input_ = ftxui::Input(&chat_input_text_, "Type a message...", chat_option);

    left_container_ = ftxui::Container::Vertical({
        name_button_,
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
    if (auth_username_.empty() || auth_password_.empty()) {
        auth_error_ = "Username and password cannot be empty";
        return false;
    }

    if (!websocket_connected_) {
        auth_error_ = "Server is not connected";
        return false;
    }

    nlohmann::json request = {
        {"type", is_register ? "register" : "login"},
        {"username", auth_username_},
        {"password", auth_password_},
    };
    auto result = websocket_->send(request.dump());
    if (!result.success) {
        auth_error_ = "Failed to send authentication request";
        return false;
    }
    auth_error_.clear();
    return true;
}

void ChatUI::ConnectWebSocket() {
    websocket_->setUrl("ws://127.0.0.1:9001");
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
                websocket_->send(nlohmann::json{
                    {"type", "resume"},
                    {"token", auth_token_},
                }.dump());
            } else {
                auth_error_.clear();
            }
            continue;
        }
        if (event.type == SocketEventType::Disconnected) {
            if (auth_state_ == AuthState::LoggedIn) {
                notification_ = "Disconnected; reconnecting...";
            } else {
                auth_error_ = "Disconnected; reconnecting...";
            }
            continue;
        }
        if (event.type == SocketEventType::Error) {
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
        if (type == "error") {
            std::string error = payload.value("error", "Server error");
            if (auth_state_ == AuthState::LoggedIn) {
                notification_ = error;
            } else {
                auth_error_ = error;
            }
            continue;
        }
        if (type == "auth_success") {
            auth_token_ = payload.value("token", "");
            my_user_id_ = payload.value("user_id", int64_t{-1});
            my_name_ = payload.value("username", auth_username_);
            auth_password_.clear();
            auth_state_ = AuthState::LoggedIn;
            websocket_authenticated_ = true;
            active_tab_index_ = 1;
            auth_error_.clear();
            notification_.clear();
            websocket_->send(nlohmann::json{{"type", "users"}}.dump());
            split_container_->TakeFocus();
            continue;
        }
        if (type == "users") {
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
                    contacts_.push_back({user_id, username, user.value("online", false), {}});
                } else {
                    contact->name = username;
                    contact->online = user.value("online", false);
                }
            }
            continue;
        }
        if (type == "chat") {
            int64_t sender_id = payload.value("sender_id", int64_t{-1});
            int64_t recipient_id = payload.value("recipient_id", int64_t{-1});
            const bool is_me = sender_id == my_user_id_;
            const int64_t contact_id = is_me ? recipient_id : sender_id;
            std::string contact_name = is_me
                ? payload.value("recipient_name", "")
                : payload.value("sender_name", "");
            auto contact = std::find_if(contacts_.begin(), contacts_.end(), [contact_id](const Contact& item) {
                return item.id == contact_id;
            });
            if (contact == contacts_.end()) {
                contacts_.push_back({contact_id, contact_name, true, {}});
                contact = std::prev(contacts_.end());
            }
            contact->messages.push_back({is_me, payload.value("content", "")});
        }
    }
    UpdateFilteredContacts();
}

void ChatUI::UpdateFilteredContacts() {
    filtered_indices_.clear();
    filtered_names_.clear();
    for (size_t i = 0; i < contacts_.size(); ++i) {
        std::string display_name = contacts_[i].name + (contacts_[i].online ? " [Online]" : " [Offline]");
        if (search_query_.empty() || contacts_[i].name.find(search_query_) != std::string::npos) {
            filtered_indices_.push_back(i);
            filtered_names_.push_back(display_name);
        }
    }
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

ftxui::Component ChatUI::GetComponent() {
    return ftxui::Renderer(root_container_, [this] {
        using namespace ftxui;

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
            auto status = auth_error_.empty()
                ? text(" ")
                : text("! " + auth_error_) | bold | color(Color::RedLight);

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
                for (const auto& [is_me, msg] : contact.messages) {
                    if (is_me) {
                        msg_elements.push_back(hbox(Elements{
                            filler(),
                            text(my_name_ + ": " + msg) | color(Color::Green)
                        }));
                    } else {
                        msg_elements.push_back(hbox(Elements{
                            text(contact.name + ": " + msg) | color(Color::Cyan),
                            filler()
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
