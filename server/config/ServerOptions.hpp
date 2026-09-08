#pragma once

#include <filesystem>

namespace cim {

struct ServerOptions {
    std::filesystem::path certificate;
    std::filesystem::path private_key;
};

inline constexpr const char* kSecureCipherSuites =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";

bool ParseServerOptions(int argc, char** argv, ServerOptions& options);
bool ValidateServerCredentials(const ServerOptions& options);

} // namespace cim
