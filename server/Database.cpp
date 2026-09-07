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
    std::lock_guard lock(mutex_);
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

    std::string registration_requests_table =
        "CREATE TABLE IF NOT EXISTS registration_requests ("
        "  username TEXT PRIMARY KEY COLLATE NOCASE,"
        "  password_hash TEXT NOT NULL,"
        "  requested_at INTEGER NOT NULL,"
        "  watch_token TEXT NOT NULL"
        ");";

    std::string registration_rejections_table =
        "CREATE TABLE IF NOT EXISTS registration_rejections ("
        "  username TEXT PRIMARY KEY COLLATE NOCASE,"
        "  reason TEXT NOT NULL,"
        "  rejected_at INTEGER NOT NULL,"
        "  watch_token TEXT NOT NULL"
        ");";

    std::string registration_approvals_table =
        "CREATE TABLE IF NOT EXISTS registration_approvals ("
        "  username TEXT PRIMARY KEY COLLATE NOCASE,"
        "  approved_at INTEGER NOT NULL,"
        "  watch_token TEXT NOT NULL"
        ");";

    if (!execute(users_table) || !execute(sessions_table) ||
        !execute(registration_requests_table) ||
        !execute(registration_rejections_table) ||
        !execute(registration_approvals_table)) {
        return false;
    }
    auto has_column = [this](const std::string& table, const std::string& column) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(
                db_, ("PRAGMA table_info(" + table + ");").c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        bool found = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            if (name && column == reinterpret_cast<const char*>(name)) {
                found = true;
                break;
            }
        }
        sqlite3_finalize(stmt);
        return found;
    };
    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    bool migration_success =
        (has_column("registration_requests", "watch_token") ||
         execute("ALTER TABLE registration_requests ADD COLUMN watch_token TEXT NOT NULL DEFAULT '';")) &&
        (has_column("registration_rejections", "watch_token") ||
         execute("ALTER TABLE registration_rejections ADD COLUMN watch_token TEXT NOT NULL DEFAULT '';")) &&
        execute("DELETE FROM registration_rejections WHERE watch_token = '';");
    if (migration_success) {
        migration_success = execute("COMMIT;");
    }
    if (!migration_success) {
        execute("ROLLBACK;");
        return false;
    }

    std::cout << "[Database] Initialized successfully at " << db_path << std::endl;
    return true;
}

int64_t Database::registerUser(const std::string& username, const std::string& password_hash) {
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
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
    std::lock_guard lock(mutex_);
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

std::vector<UserRecord> Database::getUserRecords() {
    std::lock_guard lock(mutex_);
    std::vector<UserRecord> users;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "SELECT id, username, created_at FROM users ORDER BY id;", -1, &stmt, nullptr) != SQLITE_OK) {
        return users;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* username = sqlite3_column_text(stmt, 1);
        if (username) {
            users.push_back({
                sqlite3_column_int64(stmt, 0),
                reinterpret_cast<const char*>(username),
                sqlite3_column_int64(stmt, 2),
            });
        }
    }
    sqlite3_finalize(stmt);
    return users;
}

bool Database::changePassword(const std::string& username,
                              const std::string& password_hash,
                              int64_t& user_id_out) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* user_stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "SELECT id FROM users WHERE username = ?;", -1, &user_stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(user_stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(user_stmt) != SQLITE_ROW) {
        sqlite3_finalize(user_stmt);
        return false;
    }
    user_id_out = sqlite3_column_int64(user_stmt, 0);
    sqlite3_finalize(user_stmt);

    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    sqlite3_stmt* update_stmt = nullptr;
    bool success = sqlite3_prepare_v2(
        db_, "UPDATE users SET password_hash = ? WHERE id = ?;", -1, &update_stmt, nullptr) == SQLITE_OK;
    if (success) {
        sqlite3_bind_text(update_stmt, 1, password_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update_stmt, 2, user_id_out);
        success = sqlite3_step(update_stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(update_stmt);

    sqlite3_stmt* sessions_stmt = nullptr;
    if (success) {
        success = sqlite3_prepare_v2(
            db_, "DELETE FROM sessions WHERE user_id = ?;", -1, &sessions_stmt, nullptr) == SQLITE_OK;
    }
    if (success) {
        sqlite3_bind_int64(sessions_stmt, 1, user_id_out);
        success = sqlite3_step(sessions_stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(sessions_stmt);
    if (success) {
        success = execute("COMMIT;");
        if (!success) {
            execute("ROLLBACK;");
        }
    } else {
        execute("ROLLBACK;");
    }
    return success;
}

bool Database::deleteUser(const std::string& username, int64_t& user_id_out) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* user_stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "SELECT id FROM users WHERE username = ?;", -1, &user_stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(user_stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(user_stmt) != SQLITE_ROW) {
        sqlite3_finalize(user_stmt);
        return false;
    }
    user_id_out = sqlite3_column_int64(user_stmt, 0);
    sqlite3_finalize(user_stmt);

    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    sqlite3_stmt* sessions_stmt = nullptr;
    bool success = sqlite3_prepare_v2(
        db_, "DELETE FROM sessions WHERE user_id = ?;", -1, &sessions_stmt, nullptr) == SQLITE_OK;
    if (success) {
        sqlite3_bind_int64(sessions_stmt, 1, user_id_out);
        success = sqlite3_step(sessions_stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(sessions_stmt);

    sqlite3_stmt* delete_stmt = nullptr;
    if (success) {
        success = sqlite3_prepare_v2(
            db_, "DELETE FROM users WHERE id = ?;", -1, &delete_stmt, nullptr) == SQLITE_OK;
    }
    if (success) {
        sqlite3_bind_int64(delete_stmt, 1, user_id_out);
        success = sqlite3_step(delete_stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(delete_stmt);
    if (success) {
        success = execute("COMMIT;");
        if (!success) {
            execute("ROLLBACK;");
        }
    } else {
        execute("ROLLBACK;");
    }
    return success;
}

RegistrationRequestResult Database::submitRegistration(
    const std::string& username,
    const std::string& password_hash,
    const std::string& watch_token) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    bool success = sqlite3_prepare_v2(
        db_, "DELETE FROM registration_requests WHERE requested_at <= ?;", -1, &stmt, nullptr) == SQLITE_OK;
    if (success) {
        sqlite3_bind_int64(stmt, 1, std::time(nullptr) - 86400 * 7);
        success = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!success) {
        return RegistrationRequestResult::Error;
    }
    if (!execute("BEGIN IMMEDIATE;")) {
        return RegistrationRequestResult::Error;
    }

    stmt = nullptr;
    success = sqlite3_prepare_v2(
        db_, "SELECT COUNT(*) FROM registration_requests;", -1, &stmt, nullptr) == SQLITE_OK;
    if (success) {
        success = sqlite3_step(stmt) == SQLITE_ROW;
    }
    const bool queue_full = success && sqlite3_column_int64(stmt, 0) >= 1000;
    sqlite3_finalize(stmt);
    if (!success || queue_full) {
        execute("ROLLBACK;");
        return queue_full ? RegistrationRequestResult::QueueFull : RegistrationRequestResult::Error;
    }

    stmt = nullptr;
    success = sqlite3_prepare_v2(
        db_, "SELECT 1 FROM users WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK;
    if (success) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        const int step_result = sqlite3_step(stmt);
        if (step_result == SQLITE_ROW) {
            sqlite3_finalize(stmt);
            execute("ROLLBACK;");
            return RegistrationRequestResult::UsernameExists;
        }
        success = step_result == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!success) {
        execute("ROLLBACK;");
        return RegistrationRequestResult::Error;
    }

    stmt = nullptr;
    success = sqlite3_prepare_v2(
        db_, "DELETE FROM registration_approvals WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK;
    if (success) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        success = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!success) {
        execute("ROLLBACK;");
        return RegistrationRequestResult::Error;
    }

    stmt = nullptr;
    success = sqlite3_prepare_v2(
        db_, "DELETE FROM registration_rejections WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK;
    if (success) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        success = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!success) {
        execute("ROLLBACK;");
        return RegistrationRequestResult::Error;
    }

    stmt = nullptr;
    success = sqlite3_prepare_v2(
        db_, "SELECT 1 FROM registration_requests WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK;
    if (success) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        const int step_result = sqlite3_step(stmt);
        if (step_result == SQLITE_ROW) {
            sqlite3_finalize(stmt);
            execute("ROLLBACK;");
            return RegistrationRequestResult::AlreadyPending;
        }
        success = step_result == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!success) {
        execute("ROLLBACK;");
        return RegistrationRequestResult::Error;
    }

    stmt = nullptr;
    success = sqlite3_prepare_v2(
        db_,
        "INSERT INTO registration_requests (username, password_hash, requested_at, watch_token) "
        "VALUES (?, ?, ?, ?);",
        -1,
        &stmt,
        nullptr) == SQLITE_OK;
    if (success) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, password_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, std::time(nullptr));
        sqlite3_bind_text(stmt, 4, watch_token.c_str(), -1, SQLITE_TRANSIENT);
        success = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (success) {
        success = execute("COMMIT;");
        if (!success) {
            execute("ROLLBACK;");
        }
    } else {
        execute("ROLLBACK;");
    }
    return success ? RegistrationRequestResult::Submitted : RegistrationRequestResult::Error;
}

bool Database::isRegistrationPending(const std::string& username) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT 1 FROM registration_requests WHERE username = ? AND requested_at > ?;",
            -1,
            &stmt,
            nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, std::time(nullptr) - 86400 * 7);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

bool Database::getRegistrationRequest(const std::string& username,
                                      RegistrationRequest& request_out) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT username, requested_at, watch_token FROM registration_requests "
            "WHERE username = ? AND requested_at > ?;",
            -1,
            &stmt,
            nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, std::time(nullptr) - 86400 * 7);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* stored_username = sqlite3_column_text(stmt, 0);
        if (stored_username) {
            request_out = {
                reinterpret_cast<const char*>(stored_username),
                sqlite3_column_int64(stmt, 1),
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)),
            };
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Database::getRegistrationRequests(std::vector<RegistrationRequest>& requests_out) {
    std::lock_guard lock(mutex_);
    requests_out.clear();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT username, requested_at, watch_token FROM registration_requests "
            "WHERE requested_at > ? ORDER BY requested_at, username;",
            -1,
            &stmt,
            nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(stmt, 1, std::time(nullptr) - 86400 * 7);
    int step_result = SQLITE_ROW;
    while ((step_result = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char* username = sqlite3_column_text(stmt, 0);
        if (username) {
            requests_out.push_back({
                reinterpret_cast<const char*>(username),
                sqlite3_column_int64(stmt, 1),
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)),
            });
        }
    }
    sqlite3_finalize(stmt);
    return step_result == SQLITE_DONE;
}

bool Database::approveRegistration(const std::string& username, int64_t& user_id_out) {
    std::lock_guard lock(mutex_);
    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    bool success = sqlite3_prepare_v2(
        db_,
        "SELECT username, password_hash, watch_token FROM registration_requests "
        "WHERE username = ? AND requested_at > ?;",
        -1,
        &stmt,
        nullptr) == SQLITE_OK;
    std::string password_hash;
    std::string requested_username;
    std::string watch_token;
    if (success) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, std::time(nullptr) - 86400 * 7);
        success = sqlite3_step(stmt) == SQLITE_ROW;
        if (success) {
            const unsigned char* stored_username = sqlite3_column_text(stmt, 0);
            const unsigned char* hash = sqlite3_column_text(stmt, 1);
            const unsigned char* stored_watch_token = sqlite3_column_text(stmt, 2);
            success = stored_username != nullptr && hash != nullptr && stored_watch_token != nullptr;
            if (success) {
                requested_username = reinterpret_cast<const char*>(stored_username);
                password_hash = reinterpret_cast<const char*>(hash);
                watch_token = reinterpret_cast<const char*>(stored_watch_token);
            }
        }
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    if (success) {
        success = sqlite3_prepare_v2(
            db_,
            "INSERT OR REPLACE INTO registration_approvals (username, approved_at, watch_token) "
            "VALUES (?, ?, ?);",
            -1,
            &stmt,
            nullptr) == SQLITE_OK;
    }
    if (success) {
        sqlite3_bind_text(stmt, 1, requested_username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, std::time(nullptr));
        sqlite3_bind_text(stmt, 3, watch_token.c_str(), -1, SQLITE_TRANSIENT);
        success = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    if (success) {
        success = sqlite3_prepare_v2(
            db_,
            "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?);",
            -1,
            &stmt,
            nullptr) == SQLITE_OK;
    }
    if (success) {
        sqlite3_bind_text(stmt, 1, requested_username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, password_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, std::time(nullptr));
        success = sqlite3_step(stmt) == SQLITE_DONE;
        if (success) {
            user_id_out = sqlite3_last_insert_rowid(db_);
        }
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    if (success) {
        success = sqlite3_prepare_v2(
            db_, "DELETE FROM registration_requests WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK;
    }
    if (success) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    }
    sqlite3_finalize(stmt);

    if (success) {
        success = execute("COMMIT;");
        if (!success) {
            execute("ROLLBACK;");
        }
    } else {
        execute("ROLLBACK;");
    }
    return success;
}

bool Database::rejectRegistration(const std::string& username, const std::string& reason) {
    std::lock_guard lock(mutex_);
    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    bool success = sqlite3_prepare_v2(
        db_,
        "SELECT username, watch_token FROM registration_requests WHERE username = ? AND requested_at > ?;",
        -1,
        &stmt,
        nullptr) == SQLITE_OK;
    std::string requested_username;
    std::string watch_token;
    if (success) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, std::time(nullptr) - 86400 * 7);
        success = sqlite3_step(stmt) == SQLITE_ROW;
        if (success) {
            const unsigned char* stored_username = sqlite3_column_text(stmt, 0);
            const unsigned char* stored_watch_token = sqlite3_column_text(stmt, 1);
            success = stored_username != nullptr && stored_watch_token != nullptr;
            if (success) {
                requested_username = reinterpret_cast<const char*>(stored_username);
                watch_token = reinterpret_cast<const char*>(stored_watch_token);
            }
        }
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    if (success) {
        success = sqlite3_prepare_v2(
            db_, "DELETE FROM registration_requests WHERE username = ?;", -1, &stmt, nullptr) == SQLITE_OK;
    }
    if (success) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    if (success) {
        success = sqlite3_prepare_v2(
            db_,
            "INSERT OR REPLACE INTO registration_rejections "
            "(username, reason, rejected_at, watch_token) VALUES (?, ?, ?, ?);",
            -1,
            &stmt,
            nullptr) == SQLITE_OK;
    }
    if (success) {
        sqlite3_bind_text(stmt, 1, requested_username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, std::time(nullptr));
        sqlite3_bind_text(stmt, 4, watch_token.c_str(), -1, SQLITE_TRANSIENT);
        success = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);

    if (success) {
        success = execute("COMMIT;");
        if (!success) {
            execute("ROLLBACK;");
        }
    } else {
        execute("ROLLBACK;");
    }
    return success;
}

bool Database::getRegistrationRejection(const std::string& username,
                                        const std::string& watch_token,
                                        std::string& reason_out) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT reason FROM registration_rejections WHERE username = ? AND watch_token = ?;",
            -1,
            &stmt,
            nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, watch_token.c_str(), -1, SQLITE_TRANSIENT);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* reason = sqlite3_column_text(stmt, 0);
        if (reason) {
            reason_out = reinterpret_cast<const char*>(reason);
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

bool Database::isRegistrationApproved(const std::string& username,
                                      const std::string& watch_token) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT 1 FROM registration_approvals WHERE username = ? AND watch_token = ?;",
            -1,
            &stmt,
            nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, watch_token.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

void Database::cleanupExpiredRegistrationRequests() {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "DELETE FROM registration_requests WHERE requested_at <= ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, std::time(nullptr) - 86400 * 7);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

bool Database::createSession(const std::string& token, int64_t user_id, int64_t expires_at) {
    std::lock_guard lock(mutex_);
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

bool Database::resumeSession(const std::string& token,
                             int64_t expires_at,
                             int64_t& user_id_out,
                             std::string& username_out) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT s.user_id, u.username FROM sessions s "
            "JOIN users u ON s.user_id = u.id WHERE s.token = ? AND s.expires_at > ?;",
            -1,
            &stmt,
            nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, std::time(nullptr));
    bool success = sqlite3_step(stmt) == SQLITE_ROW;
    if (success) {
        const unsigned char* username = sqlite3_column_text(stmt, 1);
        success = username != nullptr;
        if (success) {
            user_id_out = sqlite3_column_int64(stmt, 0);
            username_out = reinterpret_cast<const char*>(username);
        }
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    if (success) {
        success = sqlite3_prepare_v2(
            db_, "UPDATE sessions SET expires_at = ? WHERE token = ?;", -1, &stmt, nullptr) == SQLITE_OK;
    }
    if (success) {
        sqlite3_bind_int64(stmt, 1, expires_at);
        sqlite3_bind_text(stmt, 2, token.c_str(), -1, SQLITE_TRANSIENT);
        success = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    }
    sqlite3_finalize(stmt);
    return success;
}

bool Database::getUserByToken(const std::string& token, int64_t& user_id_out, std::string& username_out) {
    std::lock_guard lock(mutex_);
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

bool Database::deleteSession(const std::string& token) {
    std::lock_guard lock(mutex_);
    std::string sql = "DELETE FROM sessions WHERE token = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    const bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}

void Database::cleanupExpiredSessions() {
    std::lock_guard lock(mutex_);
    std::string sql = "DELETE FROM sessions WHERE expires_at <= ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, std::time(nullptr));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

} // namespace cim
