#include "ChatUI.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/component.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>

namespace cim {

inline void sendHttpPost(const std::string& endpoint, const std::string& username, const std::string& host = "127.0.0.1", int port = 9001) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return;
    }

    nlohmann::json j;
    j["username"] = username;
    std::string body = j.dump();

    std::string req = "POST " + endpoint + " HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "Connection: close\r\n\r\n" + body;

    send(sock, req.c_str(), req.size(), 0);
    close(sock);
}

inline std::vector<std::string> fetchOnlineUsers(const std::string& host = "127.0.0.1", int port = 9001) {
    std::vector<std::string> users;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return users;

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return users;
    }

    std::string req = "GET /users HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    send(sock, req.c_str(), req.size(), 0);

    std::string response;
    char buffer[4096];
    int bytes_received = 0;
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        response += buffer;
    }
    close(sock);

    size_t body_pos = response.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
        std::string body = response.substr(body_pos + 4);
        try {
            auto j = nlohmann::json::parse(body);
            if (j.is_array()) {
                for (const auto& item : j) {
                    users.push_back(item.get<std::string>());
                }
            }
        } catch (...) {}
    }

    return users;
}

ChatUI::ChatUI() {
    sendHttpPost("/join", my_name_);

    heartbeat_thread_ = std::jthread([this](std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!st.stop_requested()) {
                sendHttpPost("/heartbeat", my_name_);
            }
        }
    });

    auto online_names = fetchOnlineUsers();
    for (const auto& uname : online_names) {
        if (uname != my_name_) {
            contacts_.push_back({uname, uname, {}});
        }
    }
    if (contacts_.empty()) {
        contacts_ = {
            {"1", "Bob", {}},
            {"2", "Charlie", {}}
        };
    }

    UpdateFilteredContacts();

    ftxui::InputOption name_option;
    name_option.multiline = false;
    name_option.on_enter = [this] {
        name_input_text_.erase(std::remove(name_input_text_.begin(), name_input_text_.end(), '\n'), name_input_text_.end());
        if (!name_input_text_.empty() && name_input_text_ != my_name_) {
            sendHttpPost("/leave", my_name_);
            my_name_ = name_input_text_;
            sendHttpPost("/join", my_name_);
            editing_name_ = false;
            name_button_->TakeFocus();
        } else {
            editing_name_ = false;
            name_button_->TakeFocus();
        }
    };
    my_name_input_ = ftxui::Input(&name_input_text_, "My Name", name_option);

    ftxui::InputOption search_option;
    search_option.multiline = false;
    search_option.on_change = [this] {
        auto online_names = fetchOnlineUsers();
        contacts_.clear();
        for (const auto& uname : online_names) {
            if (uname != my_name_) {
                contacts_.push_back({uname, uname, {}});
            }
        }
        UpdateFilteredContacts();
        selected_contact_index_ = 0;
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
}

ChatUI::~ChatUI() {
    sendHttpPost("/leave", my_name_);
}

void ChatUI::UpdateFilteredContacts() {
    filtered_indices_.clear();
    filtered_names_.clear();
    for (size_t i = 0; i < contacts_.size(); ++i) {
        if (search_query_.empty() || contacts_[i].name.find(search_query_) != std::string::npos) {
            filtered_indices_.push_back(i);
            filtered_names_.push_back(contacts_[i].name);
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
    return ftxui::Renderer(split_container_, [this] {
        using namespace ftxui;

        auto online_names = fetchOnlineUsers();
        for (const auto& uname : online_names) {
            if (uname != my_name_) {
                auto it = std::find_if(contacts_.begin(), contacts_.end(), [&](const Contact& c) { return c.name == uname; });
                if (it == contacts_.end()) {
                    contacts_.push_back({uname, uname, {}});
                }
            }
        }

        UpdateFilteredContacts();
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
        }) | size(WIDTH, EQUAL, 32);

        // Right Pane
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
        if (editing_name_) {
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
