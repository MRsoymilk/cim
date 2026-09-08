#include "admin/AdminCLI.hpp"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace cim {

namespace {

constexpr int kAdminPort = 9002;

bool IsHexKey(const std::string& key) {
    return key.size() == 64 &&
        std::all_of(key.begin(), key.end(), [](unsigned char character) {
            return std::isxdigit(character);
        });
}

std::string Trim(std::string value) {
    auto is_not_space = [](unsigned char character) {
        return !std::isspace(character);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
    return value;
}

std::string ReadHiddenLine(const std::string& prompt) {
    std::cout << prompt << std::flush;
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    bool hidden = GetConsoleMode(input, &original_mode) != 0;
    if (hidden) {
        SetConsoleMode(input, original_mode & ~ENABLE_ECHO_INPUT);
    }
#else
    termios original_mode{};
    bool hidden = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original_mode) == 0;
    if (hidden) {
        termios no_echo = original_mode;
        no_echo.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &no_echo);
    }
#endif

    std::string value;
    std::getline(std::cin, value);

#ifdef _WIN32
    if (hidden) {
        SetConsoleMode(input, original_mode);
    }
#else
    if (hidden) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_mode);
    }
#endif
    std::cout << std::endl;
    return value;
}

std::string FormatTime(int64_t timestamp) {
    std::time_t time = static_cast<std::time_t>(timestamp);
    std::tm local_time{};
#ifdef _WIN32
    if (localtime_s(&local_time, &time) != 0) {
        return "unknown";
    }
#else
    if (localtime_r(&time, &local_time) == nullptr) {
        return "unknown";
    }
#endif
    std::ostringstream output;
    output << std::put_time(&local_time, "%Y-%m-%d %H:%M");
    return output.str();
}

void PrintHelp() {
    std::cout
        << "cim-server management commands:\n"
        << "  cim-server --tls-cert <PEM> --tls-key <PEM>\n"
        << "                                  Start the secure chat server\n"
        << "  cim-server users              List registered users and status\n"
        << "  cim-server pending            List registration requests\n"
        << "  cim-server approve <username> Approve a registration request\n"
        << "  cim-server reject <username>  Reject a registration request\n"
        << "  cim-server passwd <username>  Change a user's password\n"
        << "  cim-server delete <username>  Delete a user\n"
        << "  cim-server help               Show this help\n";
}

} // namespace

std::string LoadAdminKey(const std::filesystem::path& path,
                         bool create_if_missing,
                         std::string& error) {
    std::ifstream input(path);
    if (input) {
        std::string key;
        input >> key;
        if (IsHexKey(key)) {
            return key;
        }
        error = "Invalid administrator key file: " + path.string();
        return {};
    }
    if (!create_if_missing) {
        error = "Administrator key not found: " + path.string();
        return {};
    }

    unsigned char bytes[32];
    randombytes_buf(bytes, sizeof(bytes));
    std::ostringstream encoded;
    for (unsigned char byte : bytes) {
        encoded << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(byte);
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output || !(output << encoded.str() << '\n')) {
        error = "Unable to create administrator key: " + path.string();
        return {};
    }
    output.close();
#ifndef _WIN32
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
#endif
    return encoded.str();
}

bool IsAdminCommand(int argc, char** argv) {
    if (argc < 2) {
        return false;
    }
    std::string command = argv[1];
    return command == "users" || command == "pending" || command == "approve" ||
           command == "reject" || command == "passwd" || command == "delete" ||
           command == "help" || command == "--help" || command == "-h";
}

int RunAdminCommand(int argc, char** argv) {
    std::string command = argv[1];
    if (command == "help" || command == "--help" || command == "-h") {
        PrintHelp();
        return 0;
    }
    if ((command == "approve" || command == "reject" || command == "passwd" ||
         command == "delete") && argc != 3) {
        PrintHelp();
        return 2;
    }
    if ((command == "users" || command == "pending") && argc != 2) {
        PrintHelp();
        return 2;
    }

    std::string error;
    std::string key = LoadAdminKey("cim-admin.key", false, error);
    if (key.empty()) {
        std::cerr << error << std::endl;
        return 1;
    }

    nlohmann::json request = {{"token", key}, {"command", command}};
    if (command == "passwd") {
        std::string password = ReadHiddenLine("New password: ");
        std::string confirmation = ReadHiddenLine("Confirm password: ");
        if (password.empty() || password != confirmation) {
            std::cerr << "Passwords are empty or do not match" << std::endl;
            return 2;
        }
        request["username"] = argv[2];
        request["password"] = std::move(password);
    } else if (command == "delete" || command == "reject") {
        const char* action = command == "delete" ? "deletion" : "rejection";
        std::cout << "Type '" << argv[2] << "' to confirm " << action << ": " << std::flush;
        std::string confirmation;
        std::getline(std::cin, confirmation);
        if (confirmation != argv[2]) {
            std::cerr << (command == "delete" ? "Deletion cancelled" : "Rejection cancelled")
                      << std::endl;
            return 2;
        }
        request["username"] = argv[2];
        if (command == "reject") {
            std::cout << "Rejection reason: " << std::flush;
            std::string reason;
            std::getline(std::cin, reason);
            reason = Trim(std::move(reason));
            if (reason.empty()) {
                std::cerr << "Rejection cancelled: reason is required" << std::endl;
                return 2;
            }
            if (reason.size() > 500 ||
                std::any_of(reason.begin(), reason.end(), [](unsigned char character) {
                    return character < 0x20 || character == 0x7f;
                })) {
                std::cerr << "Rejection cancelled: reason must be at most 500 bytes without control characters"
                          << std::endl;
                return 2;
            }
            request["reason"] = std::move(reason);
        }
    } else if (command == "approve") {
        request["username"] = argv[2];
    }

    if (!ix::initNetSystem()) {
        std::cerr << "Unable to initialize network system" << std::endl;
        return 1;
    }

    ix::WebSocket socket;
    socket.setUrl("ws://127.0.0.1:" + std::to_string(kAdminPort));
    socket.disableAutomaticReconnection();
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::string response;
    std::string connection_error;
    socket.setOnMessageCallback([&](const ix::WebSocketMessagePtr& message) {
        if (message->type == ix::WebSocketMessageType::Open) {
            socket.send(request.dump());
            return;
        }
        std::lock_guard lock(mutex);
        if (message->type == ix::WebSocketMessageType::Message) {
            response = message->str;
            done = true;
        } else if (message->type == ix::WebSocketMessageType::Error) {
            connection_error = message->errorInfo.reason;
            done = true;
        } else if (message->type == ix::WebSocketMessageType::Close && !done) {
            connection_error = "Administrator connection closed";
            done = true;
        }
        changed.notify_all();
    });
    socket.start();
    {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return done; })) {
            connection_error = "Timed out connecting to cim-server on 127.0.0.1:9002";
        }
    }
    socket.stop();
    ix::uninitNetSystem();

    if (!connection_error.empty()) {
        std::cerr << connection_error << std::endl;
        return 1;
    }
    auto result = nlohmann::json::parse(response, nullptr, false);
    if (result.is_discarded() || !result.value("success", false)) {
        std::cerr << result.value("error", "Invalid administrator response") << std::endl;
        return 1;
    }

    if (command == "users") {
        std::cout << std::left << std::setw(8) << "ID"
                  << std::setw(24) << "USERNAME"
                  << std::setw(20) << "CREATED AT"
                  << "STATUS" << '\n';
        for (const auto& user : result.value("users", nlohmann::json::array())) {
            std::cout << std::left
                      << std::setw(8) << user.value("id", int64_t{-1})
                      << std::setw(24) << user.value("username", "")
                      << std::setw(20) << FormatTime(user.value("created_at", int64_t{0}))
                       << (user.value("online", false) ? "online" : "offline") << '\n';
        }
    } else if (command == "pending") {
        std::cout << std::left << std::setw(24) << "USERNAME"
                  << "REQUESTED AT" << '\n';
        for (const auto& registration : result.value("requests", nlohmann::json::array())) {
            std::cout << std::left
                      << std::setw(24) << registration.value("username", "")
                      << FormatTime(registration.value("requested_at", int64_t{0})) << '\n';
        }
    } else {
        std::cout << result.value("message", "Operation completed") << std::endl;
    }
    return 0;
}

} // namespace cim
