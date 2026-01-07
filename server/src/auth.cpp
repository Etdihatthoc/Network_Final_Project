#include "server/auth.hpp"

#include <chrono>
#include <random>
#include <sstream>

#include <sqlite3.h>

#include "common/crypto.hpp"

namespace quiz::server {

namespace {

std::uint64_t now_seconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

AuthService::AuthService(std::string db_path) : db_path_(std::move(db_path)) {
  open_db();
}

AuthService::~AuthService() {
  if (db_) sqlite3_close(db_);
}

bool AuthService::open_db() {
  std::lock_guard<std::mutex> lock(mtx_);
  if (db_) return true;
  if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
    db_ = nullptr;
    return false;
  }
  return true;
}

std::string AuthService::random_token() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  std::ostringstream oss;
  oss << std::hex << dist(gen) << dist(gen);
  return oss.str();
}

std::optional<int> AuthService::register_user(const std::string& username,
                                              const std::string& password,
                                              const std::string& full_name,
                                              const std::string& email,
                                              const std::string& role,
                                              std::string* error) {
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mtx_);
  const char* sql = "INSERT INTO users(username, pass_hash, role, full_name, email, created_at) "
                    "VALUES(?,?,?,?,?, ?);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }
  auto hashed = hash_password(password);
  sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, hashed.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, full_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, email.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(now_seconds()));

  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    if (error) *error = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  int new_id = static_cast<int>(sqlite3_last_insert_rowid(db_));
  sqlite3_finalize(stmt);
  return new_id;
}

std::optional<SessionInfo> AuthService::login(const std::string& username,
                                              const std::string& password,
                                              std::uint64_t ttl_seconds,
                                              std::string* error) {
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mtx_);

  const char* sql_user = "SELECT id, pass_hash, role, full_name, email FROM users WHERE username = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql_user, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    if (error) *error = "User not found";
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  int uid = sqlite3_column_int(stmt, 0);
  std::string stored_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  std::string role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  sqlite3_finalize(stmt);

  if (!verify_password(password, stored_hash)) {
    if (error) *error = "Invalid credentials";
    return std::nullopt;
  }

  std::string token = random_token();
  std::uint64_t now = now_seconds();
  std::uint64_t expires = now + ttl_seconds;

  const char* sql_sess = "INSERT INTO sessions(user_id, token, expires_at, created_at) VALUES(?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql_sess, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }
  sqlite3_bind_int(stmt, 1, uid);
  sqlite3_bind_text(stmt, 2, token.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(expires));
  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(now));
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    if (error) *error = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  sqlite3_finalize(stmt);

  SessionInfo session{uid, username, role, token, expires};
  return session;
}

bool AuthService::logout(const std::string& token, std::string* error) {
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return false;
  }
  std::lock_guard<std::mutex> lock(mtx_);
  const char* sql = "DELETE FROM sessions WHERE token = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return true;
}

std::optional<SessionInfo> AuthService::validate(const std::string& token, std::string* error) {
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mtx_);

  const char* sql = "SELECT s.user_id, u.username, u.role, s.expires_at "
                    "FROM sessions s JOIN users u ON s.user_id = u.id "
                    "WHERE s.token = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    if (error) *error = "Session not found";
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  int uid = sqlite3_column_int(stmt, 0);
  std::string username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  std::string role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  std::uint64_t exp = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 3));
  sqlite3_finalize(stmt);

  std::uint64_t now = now_seconds();
  if (now > exp) {
    if (error) *error = "Session expired";
    // Clean up expired session
    sqlite3_stmt* del = nullptr;
    const char* del_sql = "DELETE FROM sessions WHERE token = ?;";
    if (sqlite3_prepare_v2(db_, del_sql, -1, &del, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(del, 1, token.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(del);
      sqlite3_finalize(del);
    }
    return std::nullopt;
  }

  SessionInfo session{uid, username, role, token, exp};
  return session;
}

}  // namespace quiz::server
