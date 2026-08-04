#pragma once

#include <sqlite3.h>
#include <string>
#include <vector>
#include <utility>
#include <optional>
#include <cstdint>

namespace cim {

class Database {
public:
    Database();
    ~Database();

    bool init(const std::string& db_path = "cim.db");
    
    // User operations
    int64_t registerUser(const std::string& username, const std::string& password_hash);
    bool getUser(const std::string& username, int64_t& id_out, std::string& password_hash_out);
    bool getUserById(int64_t id, std::string& username_out);
    std::vector<std::pair<int64_t, std::string>> getAllUsers();

    // Session operations
    bool createSession(const std::string& token, int64_t user_id, int64_t expires_at);
    bool getUserByToken(const std::string& token, int64_t& user_id_out, std::string& username_out);
    void deleteSession(const std::string& token);
    void cleanupExpiredSessions();

private:
    sqlite3* db_ = nullptr;
    bool execute(const std::string& sql);
};

} // namespace cim
