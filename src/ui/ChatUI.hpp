#pragma once

#include "ftxui/component/component.hpp"
#include <string>
#include <vector>
#include <thread>

namespace cim {

struct Contact {
    std::string id;
    std::string name;
    std::vector<std::pair<bool, std::string>> messages;
};

class ChatUI {
public:
    ChatUI();
    ~ChatUI();
    ftxui::Component GetComponent();

private:
    std::string my_name_ = "Alice";
    bool editing_name_ = false;
    std::string name_input_text_ = "Alice";

    std::string search_query_ = "";
    
    std::vector<Contact> contacts_;
    int selected_contact_index_ = 0;

    std::string chat_input_text_ = "";
    std::string notification_;

    int active_pane_ = 0; // 0 = left, 1 = right

    ftxui::Component my_name_input_;
    ftxui::Component search_input_;
    ftxui::Component chat_input_;
    ftxui::Component name_button_;
    ftxui::Component contact_menu_;
    ftxui::Component left_container_;
    ftxui::Component right_container_;
    ftxui::Component split_container_;

    std::vector<int> filtered_indices_;
    std::vector<std::string> filtered_names_;

    std::jthread heartbeat_thread_;

    void UpdateFilteredContacts();
    void SendMessage();
};

} // namespace cim
