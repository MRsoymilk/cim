#include "ui/chat/ChatUI.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace cim {

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
        } else if (!connection_.IsAuthenticated()) {
            notification_ = "WebSocket is not authenticated";
        } else {
            nlohmann::json message = {
                {"type", "chat"},
                {"recipient_id", contacts_[actual_idx].id},
                {"content", chat_input_text_},
            };
            if (connection_.Send(message.dump())) {
                notification_.clear();
                chat_input_text_.clear();
            } else {
                notification_ = "Failed to send WebSocket message";
            }
        }
    }
}

} // namespace cim
