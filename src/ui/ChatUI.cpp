#include "ChatUI.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/component.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

namespace cim {

class WinSockInitializer {
public:
    WinSockInitializer() {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    }
    ~WinSockInitializer() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

inline void closeSocket(int sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

inline int createSocket() {
#ifdef _WIN32
    return static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
#else
    return socket(AF_INET, SOCK_STREAM, 0);
#endif
}

inline bool sendAuthRequest(bool is_register, const std::string& username, const std::string& password, std::string& token_out, int64_t& id_out, std::string& error_out, const std::string& host = "127.0.0.1", int port = 9001) {
    static WinSockInitializer ws_init;
    int sock = createSocket();
    if (sock < 0) {
        error_out = "Socket creation failed";
        return false;
    }

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr);

#ifdef _WIN32
    DWORD tv = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        closeSocket(sock);
        error_out = "Connection to server failed";
        return false;
    }

    nlohmann::json j;
    j["username"] = username;
    j["password"] = password;
    std::string body = j.dump();

    std::string endpoint = is_register ? "/auth/register" : "/auth/login";
    std::string req = "POST " + endpoint + " HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "Connection: close\r\n\r\n" + body;

    send(sock, req.c_str(), (int)req.size(), 0);

    std::string response;
    char buffer[4096];
    int bytes_received = 0;
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        response += buffer;
    }
    closeSocket(sock);

    size_t body_pos = response.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
        std::string resp_body = response.substr(body_pos + 4);
        try {
            auto res_json = nlohmann::json::parse(resp_body);
            if (res_json.contains("success") && res_json["success"].get<bool>()) {
                token_out = res_json.value("token", "");
                id_out = res_json.value("user_id", -1);
                return true;
            } else {
                error_out = res_json.value("error", "Authentication failed");
            }
        } catch (...) {
            error_out = "Invalid response from server";
        }
    } else {
        error_out = "No response from server";
    }
    return false;
}

inline void sendAuthenticatedPost(const std::string& endpoint, const std::string& token, const std::string& host = "127.0.0.1", int port = 9001) {
    static WinSockInitializer ws_init;
    int sock = createSocket();
    if (sock < 0) return;

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr);

#ifdef _WIN32
    DWORD tv = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        closeSocket(sock);
        return;
    }

    std::string req = "POST " + endpoint + " HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "Authorization: Bearer " + token + "\r\n"
                      "Content-Length: 0\r\n"
                      "Connection: close\r\n\r\n";

    send(sock, req.c_str(), (int)req.size(), 0);
    closeSocket(sock);
}

inline std::vector<Contact> fetchOnlineUsersWithAuth(const std::string& token, const std::string& current_username, const std::string& host = "127.0.0.1", int port = 9001) {
    static WinSockInitializer ws_init;
    std::vector<Contact> contacts;
    int sock = createSocket();
    if (sock < 0) return contacts;

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr);

#ifdef _WIN32
    DWORD tv = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        closeSocket(sock);
        return contacts;
    }

    std::string req = "GET /users HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "Authorization: Bearer " + token + "\r\n"
                      "Connection: close\r\n\r\n";

    send(sock, req.c_str(), (int)req.size(), 0);

    std::string response;
    char buffer[4096];
    int bytes_received = 0;
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        response += buffer;
    }
    closeSocket(sock);

    size_t body_pos = response.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
        std::string body = response.substr(body_pos + 4);
        try {
            auto j = nlohmann::json::parse(body);
            if (j.is_array()) {
                for (const auto& item : j) {
                    int64_t id = item.value("id", -1);
                    std::string uname = item.value("username", "");
                    bool online = item.value("online", false);
                    if (!uname.empty() && uname != current_username) {
                        contacts.push_back({id, uname, online, {}});
                    }
                }
            }
        } catch (...) {}
    }

    return contacts;
}

ChatUI::ChatUI() {
    ftxui::InputOption user_option;
    user_option.multiline = false;
    login_username_input_ = ftxui::Input(&auth_username_, "Username", user_option);

    ftxui::InputOption pass_option;
    pass_option.multiline = false;
    pass_option.password = true;
    login_password_input_ = ftxui::Input(&auth_password_, "Password", pass_option);

    login_btn_ = ftxui::Button("Submit", [this] {
        PerformAuth(auth_state_ == AuthState::Register);
    });

    switch_btn_ = ftxui::Button("Switch Mode", [this] {
        auth_state_ = (auth_state_ == AuthState::Login) ? AuthState::Register : AuthState::Login;
        auth_error_.clear();
    });

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
        RefreshContacts();
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
}

ChatUI::~ChatUI() {
    if (auth_state_ == AuthState::LoggedIn && !auth_token_.empty()) {
        sendAuthenticatedPost("/leave", auth_token_);
    }
}

bool ChatUI::PerformAuth(bool is_register) {
    if (auth_username_.empty() || auth_password_.empty()) {
        auth_error_ = "Username and password cannot be empty";
        return false;
    }

    int64_t uid = -1;
    std::string token;
    std::string err;
    if (sendAuthRequest(is_register, auth_username_, auth_password_, token, uid, err)) {
        auth_token_ = token;
        my_user_id_ = uid;
        my_name_ = auth_username_;
        auth_state_ = AuthState::LoggedIn;
        active_tab_index_ = 1; // Switch to chat tab
        auth_error_.clear();

        sendAuthenticatedPost("/join", auth_token_);

        heartbeat_thread_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                if (!st.stop_requested()) {
                    sendAuthenticatedPost("/heartbeat", auth_token_);
                }
            }
        });

        RefreshContacts();
        split_container_->TakeFocus();
        return true;
    } else {
        auth_error_ = err;
        return false;
    }
}

void ChatUI::RefreshContacts() {
    if (auth_state_ != AuthState::LoggedIn) return;
    auto online_contacts = fetchOnlineUsersWithAuth(auth_token_, my_name_);
    
    for (const auto& new_c : online_contacts) {
        auto it = std::find_if(contacts_.begin(), contacts_.end(), [&](const Contact& c) { return c.id == new_c.id; });
        if (it != contacts_.end()) {
            it->online = new_c.online;
            it->name = new_c.name;
        } else {
            contacts_.push_back(new_c);
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
        } else {
            notification_.clear();
            contacts_[actual_idx].messages.push_back({true, chat_input_text_});
            chat_input_text_.clear();
        }
    }
}

ftxui::Component ChatUI::GetComponent() {
    return ftxui::Renderer(root_container_, [this] {
        using namespace ftxui;

        if (auth_state_ != AuthState::LoggedIn) {
            Elements elements = Elements{
                text("=== cim - Command Instant Messenger ===") | bold | color(Color::Cyan) | center,
                separator(),
                text(auth_state_ == AuthState::Login ? "Please Login" : "Please Register") | bold | center,
                separator(),
                hbox(Elements{text(" Username: "), login_username_input_->Render()}) | border,
                hbox(Elements{text(" Password: "), login_password_input_->Render()}) | border,
            };

            if (!auth_error_.empty()) {
                elements.push_back(text(auth_error_) | color(Color::Red) | bold | center);
            }

            elements.push_back(separator());
            elements.push_back(hbox(Elements{login_btn_->Render(), text("   "), switch_btn_->Render()}) | center);

            return vbox(elements) | border | center;
        }

        RefreshContacts();
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
