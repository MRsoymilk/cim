#include "ClientConfig.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace cim {

namespace {

std::filesystem::path ConfigPath() {
#ifdef _WIN32
    if (const char* app_data = std::getenv("APPDATA")) {
        return std::filesystem::path(app_data) / "cim" / "config.json";
    }
#else
    if (const char* xdg_config = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg_config) / "cim" / "config.json";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "cim" / "config.json";
    }
#endif
    return std::filesystem::current_path() / "cim-config.json";
}

} // namespace

ClientConfig ClientConfig::Load() {
    ClientConfig config;
    std::ifstream input(ConfigPath());
    if (!input) {
        return config;
    }

    try {
        nlohmann::json document;
        input >> document;
        std::string host = document.value("host", config.host);
        int port = document.value("port", static_cast<int>(config.port));
        if (!host.empty() && port >= 1 && port <= 65535) {
            config.host = std::move(host);
            config.port = static_cast<uint16_t>(port);
            config.username = document.value("username", "");
            config.session_token = document.value("session_token", "");
        }
    } catch (const nlohmann::json::exception&) {
        return ClientConfig{};
    }
    return config;
}

bool ClientConfig::Save(std::string& error) const {
    try {
        const auto path = ConfigPath();
        std::filesystem::create_directories(path.parent_path());
#ifndef _WIN32
        {
            std::ofstream create(path, std::ios::app);
            if (!create) {
                error = "Unable to create the configuration file";
                return false;
            }
        }
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
#endif
        std::ofstream output(path, std::ios::trunc);
        if (!output) {
            error = "Unable to open the configuration file";
            return false;
        }
        output << nlohmann::json{
            {"host", host},
            {"port", port},
            {"username", username},
            {"session_token", session_token},
        }.dump(2) << '\n';
        if (!output) {
            error = "Unable to write the configuration file";
            return false;
        }
        output.close();
#ifndef _WIN32
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
#endif
        return true;
    } catch (const std::filesystem::filesystem_error& exception) {
        error = exception.what();
        return false;
    }
}

std::string ClientConfig::WebSocketUrl() const {
    std::string url_host = host;
    if (host.find(':') != std::string::npos &&
        !(host.starts_with('[') && host.ends_with(']'))) {
        url_host = '[' + host + ']';
    }
    return "ws://" + url_host + ':' + std::to_string(port);
}

} // namespace cim
