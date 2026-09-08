#pragma once

#include <cstdint>
#include <string>

namespace cim {

struct ClientConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 9001;
    std::string ca_source = "BUNDLED";
    std::string custom_ca_path;
    std::string username;
    std::string session_token;

    static ClientConfig Load();
    bool Save(std::string& error) const;
    std::string WebSocketUrl() const;
    std::string TrustedCaData() const;
};

} // namespace cim
