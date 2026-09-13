#include "ui/chat/ChatUI.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/string.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace cim {

namespace {

constexpr int kAuthStatusWidth = 46;

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

ftxui::Element ScrollingStatus(std::string message, ftxui::Color color) {
    const int content_width = ftxui::string_width(message);
    auto content = ftxui::text(std::move(message)) | ftxui::bold | ftxui::color(color);
    if (content_width <= kAuthStatusWidth) {
        return content |
            ftxui::center |
            ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kAuthStatusWidth);
    }

    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    const auto steps = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 150;
    const int distance = content_width - kAuthStatusWidth;
    const int phase = static_cast<int>(steps % (distance * 2));
    const int offset = phase <= distance ? phase : distance * 2 - phase;
    const float position = static_cast<float>(offset) / static_cast<float>(distance);
    return content |
        ftxui::focusPositionRelative(position, 0.0f) |
        ftxui::frame |
        ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kAuthStatusWidth);
}

} // namespace

ftxui::Component ChatUI::GetComponent() {
    return ftxui::Renderer(root_container_, [this] {
        using namespace ftxui;
        animate_auth_status_.store(false);

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
            auto ca_source_field = vbox(Elements{
                text("TRUSTED CA") | color(Color::GrayDark),
                settings_ca_source_toggle_->Render() | borderLight,
            });
            auto custom_ca_field = vbox(Elements{
                text("CUSTOM CA CERTIFICATE") | color(Color::GrayDark),
                hbox(Elements{
                    text(" > ") | bold | color(Color::Cyan),
                    settings_custom_ca_input_->Render() | flex,
                }) | borderLight,
            });
            const std::string connection_error = !settings_error_.empty()
                ? settings_error_
                : (!connection_.IsConnected() ? auth_error_ : "");
            auto status = connection_.IsConnected()
                ? text("CONNECTED") | bold | color(Color::Green)
                : connection_error.empty()
                    ? text("CONNECTING...") | color(Color::Yellow)
                    : text("CONNECTION ERROR") | bold | color(Color::RedLight);
            auto error = connection_error.empty()
                ? text(" ")
                : paragraph("! " + connection_error) | bold | color(Color::RedLight);

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
                text(" "),
                ca_source_field,
                text(" "),
                custom_ca_field,
                error | center,
                settings_save_btn_->Render() | size(WIDTH, EQUAL, 48) | hcenter,
                text(" "),
                settings_back_btn_->Render() | center,
            }) | size(WIDTH, EQUAL, 68) | borderRounded;

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
            Element status = text(" ") | size(WIDTH, EQUAL, kAuthStatusWidth);
            bool animate_status = false;
            if (!auth_error_.empty()) {
                const std::string message = "! " + auth_error_;
                animate_status = string_width(message) > kAuthStatusWidth;
                status = ScrollingStatus(message, Color::RedLight);
            } else if (!auth_notice_.empty()) {
                animate_status = string_width(auth_notice_) > kAuthStatusWidth;
                status = ScrollingStatus(auth_notice_, Color::GreenLight);
            }
            animate_auth_status_.store(animate_status);

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
                remember_password_checkbox_->Render() | center,
                status,
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
