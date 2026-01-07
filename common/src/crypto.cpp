#include "common/crypto.hpp"

#include <functional>
#include <sstream>
#include <string>

namespace quiz {

namespace {
std::string to_hex(std::size_t value) {
  std::ostringstream oss;
  oss << std::hex << value;
  return oss.str();
}
}  // namespace

std::string hash_password(const std::string& password, const std::string& salt) {
  std::hash<std::string> h;
  auto v = h(salt + password);
  return to_hex(v);
}

bool verify_password(const std::string& password, const std::string& stored_hash, const std::string& salt) {
  return hash_password(password, salt) == stored_hash;
}

}  // namespace quiz
