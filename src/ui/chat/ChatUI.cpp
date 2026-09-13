#include "ui/chat/ChatUI.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include <algorithm>
#include <utility>

namespace cim {

ChatUI::ChatUI(ftxui::Closure request_refresh, std::string trusted_ca_override)
    : connection_(std::move(request_refresh)),
      trusted_ca_override_(std::move(trusted_ca_override)) {
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
    settings_ca_source_toggle_ = ftxui::Toggle(
        &settings_ca_sources_, &settings_ca_source_index_);
    settings_custom_ca_input_ = ftxui::Input(
        &settings_custom_ca_path_, "/path/to/ca.crt", settings_input_option);

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
        settings_ca_source_toggle_,
        settings_custom_ca_input_,
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
    connection_.Stop();
}

} // namespace cim
