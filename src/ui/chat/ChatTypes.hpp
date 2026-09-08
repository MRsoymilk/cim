#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cim {

struct ChatMessage {
    bool is_me;
    std::string content;
    int64_t sent_at;
};

struct Contact {
    int64_t id;
    std::string name;
    bool online;
    bool unread;
    std::vector<ChatMessage> messages;
};

enum class AuthState {
    Login,
    Register,
    LoggedIn
};

} // namespace cim
