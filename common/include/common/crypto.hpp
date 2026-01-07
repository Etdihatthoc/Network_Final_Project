#pragma once

#include <string>

namespace quiz {

// Simple non-cryptographic hash for demo purposes only.
// DO NOT USE in production.
std::string hash_password(const std::string& password, const std::string& salt = "quiz_salt");

bool verify_password(const std::string& password, const std::string& stored_hash, const std::string& salt = "quiz_salt");

}  // namespace quiz
