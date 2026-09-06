#pragma once

#include <cstdint>
#include <string>

namespace cim {

struct ClientConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 9001;

    static ClientConfig Load();
    bool Save(std::string& error) const;
    std::string WebSocketUrl() const;
};

} // namespace cim
