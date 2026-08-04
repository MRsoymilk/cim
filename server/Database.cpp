#include "Database.hpp"
#include <iostream>
#include <ctime>

namespace cim {

Database::Database() = default;

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

bool Database::execute(const std::string& sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[Database] SQL error: " << (err_msg ? err_msg : "unknown") << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

bool Database::init(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "[Database] Cannot open database: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Create tables
    std::string users_table = 
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT NOT NULL COLLATE NOCASE UNIQUE,"
        "  password_hash TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ");";

    std::string sessions_table = 
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  token TEXT PRIMARY KEY,"
        "  user_id INTEGER NOT NULL,"
        "  expires_at INTEGER NOT NULL,"
        "  FOREIGN KEY(user_id) REFERENCES users(id)"
        ");";

    if (!execute(users_table) || !execute(sessions_table)) {
        return false;
    }

    std::cout << "[Database] Initialized successfully at " << db_path << std::endl;
    return true;
}

int64_t Database::registerUser(const std::string& username, const std::string& password_hash) {
    std::string sql = "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password_hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, std::time(nullptr));

    rc = sqlite3_step(stmt);
    int64_t user_id = -1;
    if (rc == SQLITE_DONE) {
        user_id = sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(stmt);
    return user_id;
}

bool Database::getUser(const std::string& username, int64_t& id_out, std::string& password_hash_out) {
    std::string sql = "SELECT id, password_hash FROM users WHERE username = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id_out = sqlite3_column_int64(stmt, 0);
        const unsigned char* hash = sqlite3_column_text(stmt, 1);
        if (hash) {
            password_hash_out = reinterpret_cast<const char*>(hash);
        }
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Database::getUserById(int64_t id, std::string& username_out) {
    std::string sql = "SELECT username FROM users WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, id);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* uname = sqlite3_column_text(stmt, 0);
        if (uname) {
            username_out = reinterpret_cast<const char*>(uname);
        }
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

std::vector<std::pair<int64_t, std::string>> Database::getAllUsers() {
    std::vector<std::pair<int64_t, std::string>> users;
    std::string sql = "SELECT id, username FROM users;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return users;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        const unsigned char* uname = sqlite3_column_text(stmt, 1);
        if (uname) {
            users.emplace_back(id, reinterpret_cast<const char*>(uname));
        }
    }
    sqlite3_finalize(stmt);
    return users;
}

bool Database::createSession(const std::string& token, int64_t user_id, int64_t expires_at) {
    std::string sql = "INSERT OR REPLACE INTO sessions (token, user_id, expires_at) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, user_id);
    sqlite3_bind_int64(stmt, 3, expires_at);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return success;
}

bool Database::getUserByToken(const std::string& token, int64_t& user_id_out, std::string& username_out) {
    std::string sql = "SELECT s.user_id, u.username FROM sessions s JOIN users u ON s.user_id = u.id WHERE s.token = ? AND s.expires_at > ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, std::time(nullptr));

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        user_id_out = sqlite3_column_int64(stmt, 0);
        const unsigned char* uname = sqlite3_column_text(stmt, 1);
        if (uname) {
            username_out = reinterpret_cast<const char*>(uname);
        }
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

void Database::deleteSession(const std::string& token) {
    std::string sql = "DELETE FROM sessions WHERE token = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::cleanupExpiredSessions() {
    std::string sql = "DELETE FROM sessions WHERE expires_at <= ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

} // namespace cim
