#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "common/message.hpp"

namespace quiz::server {

struct SessionInfo {
  int user_id{};
  std::string username;
  std::string role;
  std::string token;
  std::uint64_t expires_at{};
};

class AuthService {
 public:
  explicit AuthService(std::string db_path);
  ~AuthService();

  AuthService(const AuthService&) = delete;
  AuthService& operator=(const AuthService&) = delete;

  std::optional<int> register_user(const std::string& username,
                                   const std::string& password,
                                   const std::string& full_name,
                                   const std::string& email,
                                   const std::string& role = "STUDENT",
                                   std::string* error = nullptr);

  std::optional<SessionInfo> login(const std::string& username,
                                   const std::string& password,
                                   std::uint64_t ttl_seconds,
                                   std::string* error = nullptr);

  bool logout(const std::string& token, std::string* error = nullptr);

  std::optional<SessionInfo> validate(const std::string& token, std::string* error = nullptr);

 private:
  bool open_db();
  std::string random_token();

  std::string db_path_;
  sqlite3* db_{nullptr};
  std::mutex mtx_;
};

}  // namespace quiz::server
