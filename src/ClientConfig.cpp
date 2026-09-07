#include "ClientConfig.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

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

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
    }
#else
    std::error_code error;
    auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !executable.empty()) {
        return executable.parent_path();
    }
#endif
    return std::filesystem::current_path();
}

bool IsValidCaSource(const std::string& source) {
    return source == "BUNDLED" || source == "SYSTEM" || source == "CUSTOM";
}

std::filesystem::path Utf8Path(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()),
        reinterpret_cast<const char8_t*>(value.data() + value.size())));
}

std::string ReadCertificate(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    constexpr std::streamoff kMaximumCaBytes = 4 * 1024 * 1024;
    if (size <= 0 || size > kMaximumCaBytes) {
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::string certificate(static_cast<size_t>(size), '\0');
    if (!input.read(certificate.data(), size)) {
        return {};
    }
    if (certificate.find("-----BEGIN CERTIFICATE-----") == std::string::npos ||
        certificate.find("PRIVATE KEY") != std::string::npos ||
        certificate == "NONE" || certificate == "SYSTEM") {
        return {};
    }
    return certificate;
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
            const std::string ca_source = document.value("ca_source", config.ca_source);
            if (IsValidCaSource(ca_source)) {
                config.ca_source = ca_source;
                config.custom_ca_path = document.value("custom_ca_path", "");
                if (config.ca_source == "CUSTOM" &&
                    (config.custom_ca_path.empty() || config.custom_ca_path == "NONE" ||
                     config.custom_ca_path == "SYSTEM")) {
                    config.ca_source = "BUNDLED";
                    config.custom_ca_path.clear();
                }
            }
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
            {"ca_source", ca_source},
            {"custom_ca_path", custom_ca_path},
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
    return "wss://" + url_host + ':' + std::to_string(port);
}

std::string ClientConfig::TrustedCaData() const {
    if (ca_source == "SYSTEM") {
        return "SYSTEM";
    }
    if (ca_source == "CUSTOM" && !custom_ca_path.empty() &&
        custom_ca_path != "NONE" && custom_ca_path != "SYSTEM") {
        return ReadCertificate(Utf8Path(custom_ca_path));
    }
    return ReadCertificate(ExecutableDirectory() / "cim-ca.crt");
}

} // namespace cim
