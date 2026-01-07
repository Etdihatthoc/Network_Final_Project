#include "common/codec.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <unistd.h>

#include <nlohmann/json.hpp>

namespace quiz {
namespace {

bool is_valid_utf8(const std::string& s) {
  const unsigned char* bytes =
      reinterpret_cast<const unsigned char*>(s.data());
  std::size_t len = s.size();
  std::size_t i = 0;
  while (i < len) {
    unsigned char c = bytes[i];
    std::size_t remaining = 0;
    if (c <= 0x7F) {
      remaining = 0;
    } else if ((c >> 5) == 0x6) {
      remaining = 1;
      if ((c & 0x1E) == 0) return false;
    } else if ((c >> 4) == 0xE) {
      remaining = 2;
    } else if ((c >> 3) == 0x1E) {
      remaining = 3;
    } else {
      return false;
    }
    if (i + remaining >= len) return false;
    for (std::size_t j = 1; j <= remaining; ++j) {
      if ((bytes[i + j] >> 6) != 0x2) return false;
    }
    i += remaining + 1;
  }
  return true;
}

std::uint32_t to_be32(std::uint32_t value) {
  return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) |
         ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

std::uint32_t from_be32(std::uint32_t value) {
  return to_be32(value);
}

}  // namespace

// Message helpers implementation
std::string to_string(MessageType type) {
  switch (type) {
    case MessageType::Request:
      return "REQUEST";
    case MessageType::Response:
      return "RESPONSE";
    case MessageType::Notification:
      return "NOTIFICATION";
  }
  return "REQUEST";
}

std::string to_string(Status status) {
  switch (status) {
    case Status::None:
      return "";
    case Status::Success:
      return "SUCCESS";
    case Status::Error:
      return "ERROR";
  }
  return "";
}

std::optional<MessageType> message_type_from_string(const std::string& value) {
  if (value == "REQUEST") return MessageType::Request;
  if (value == "RESPONSE") return MessageType::Response;
  if (value == "NOTIFICATION") return MessageType::Notification;
  return std::nullopt;
}

std::optional<Status> status_from_string(const std::string& value) {
  if (value.empty()) return Status::None;
  if (value == "SUCCESS") return Status::Success;
  if (value == "ERROR") return Status::Error;
  return std::nullopt;
}

std::optional<Message> message_from_json(const nlohmann::json& j,
                                         std::string& error) {
  if (!j.is_object()) {
    error = "Message must be a JSON object";
    return std::nullopt;
  }

  Message msg;

  if (!j.contains("message_type") || !j["message_type"].is_string()) {
    error = "message_type missing or not string";
    return std::nullopt;
  }
  auto mt = message_type_from_string(j["message_type"].get<std::string>());
  if (!mt) {
    error = "invalid message_type";
    return std::nullopt;
  }
  msg.type = *mt;

  if (!j.contains("action") || !j["action"].is_string() ||
      j["action"].get<std::string>().empty()) {
    error = "action missing or empty";
    return std::nullopt;
  }
  msg.action = j["action"].get<std::string>();

  if (!j.contains("timestamp") || !j["timestamp"].is_number_unsigned()) {
    error = "timestamp missing or not unsigned number";
    return std::nullopt;
  }
  msg.timestamp = j["timestamp"].get<std::uint64_t>();

  if (j.contains("session_id")) {
    if (!j["session_id"].is_string()) {
      error = "session_id must be string";
      return std::nullopt;
    }
    msg.session_id = j["session_id"].get<std::string>();
  }

  if (j.contains("data")) {
    if (!j["data"].is_object()) {
      error = "data must be JSON object";
      return std::nullopt;
    }
    msg.data = j["data"];
  } else {
    msg.data = nlohmann::json::object();
  }

  if (j.contains("status")) {
    if (!j["status"].is_string()) {
      error = "status must be string";
      return std::nullopt;
    }
    auto st = status_from_string(j["status"].get<std::string>());
    if (!st) {
      error = "invalid status";
      return std::nullopt;
    }
    msg.status = *st;
  }

  if (j.contains("error_code")) {
    if (!j["error_code"].is_string()) {
      error = "error_code must be string";
      return std::nullopt;
    }
    msg.error_code = j["error_code"].get<std::string>();
  }

  if (j.contains("error_message")) {
    if (!j["error_message"].is_string()) {
      error = "error_message must be string";
      return std::nullopt;
    }
    msg.error_message = j["error_message"].get<std::string>();
  }

  // For RESPONSE messages, status should be present.
  if (msg.type == MessageType::Response && msg.status == Status::None) {
    error = "response requires status";
    return std::nullopt;
  }

  return msg;
}

nlohmann::json message_to_json(const Message& msg) {
  nlohmann::json j;
  j["message_type"] = to_string(msg.type);
  j["action"] = msg.action;
  j["timestamp"] = msg.timestamp;
  if (!msg.session_id.empty()) j["session_id"] = msg.session_id;
  j["data"] = msg.data.is_null() ? nlohmann::json::object() : msg.data;
  if (msg.status != Status::None) j["status"] = to_string(msg.status);
  if (!msg.error_code.empty()) j["error_code"] = msg.error_code;
  if (!msg.error_message.empty()) j["error_message"] = msg.error_message;
  return j;
}

ssize_t read_exact(int fd, void* buffer, std::size_t length) {
  auto* out = static_cast<std::uint8_t*>(buffer);
  std::size_t total = 0;
  while (total < length) {
    ssize_t n = ::read(fd, out + total, length - total);
    if (n == 0) {
      return static_cast<ssize_t>(total);  // EOF
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    total += static_cast<std::size_t>(n);
  }
  return static_cast<ssize_t>(total);
}

ssize_t write_exact(int fd, const void* buffer, std::size_t length) {
  const auto* in = static_cast<const std::uint8_t*>(buffer);
  std::size_t total = 0;
  while (total < length) {
    ssize_t n = ::write(fd, in + total, length - total);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    total += static_cast<std::size_t>(n);
  }
  return static_cast<ssize_t>(total);
}

std::vector<std::uint8_t> encode_frame(const Message& msg, std::string& error) {
  nlohmann::json payload = message_to_json(msg);
  std::string json_str = payload.dump();
  if (json_str.size() > kMaxPayloadSize) {
    error = "payload too large";
    return {};
  }
  if (!is_valid_utf8(json_str)) {
    error = "payload not valid UTF-8";
    return {};
  }

  std::vector<std::uint8_t> frame;
  frame.resize(kFramePrefixBytes + json_str.size());
  std::uint32_t len = static_cast<std::uint32_t>(json_str.size());
  len = to_be32(len);
  std::memcpy(frame.data(), &len, sizeof(len));
  std::memcpy(frame.data() + kFramePrefixBytes, json_str.data(),
              json_str.size());
  return frame;
}

bool decode_frame(const std::vector<std::uint8_t>& frame, Message& out,
                  std::string& error) {
  if (frame.size() < kFramePrefixBytes) {
    error = "frame too small";
    return false;
  }
  std::uint32_t be_len = 0;
  std::memcpy(&be_len, frame.data(), sizeof(be_len));
  const std::uint32_t payload_len = from_be32(be_len);
  if (payload_len > kMaxPayloadSize) {
    error = "payload too large";
    return false;
  }
  if (frame.size() != kFramePrefixBytes + payload_len) {
    error = "payload length mismatch";
    return false;
  }

  std::string payload(reinterpret_cast<const char*>(frame.data() + kFramePrefixBytes),
                      payload_len);
  if (!is_valid_utf8(payload)) {
    error = "payload not valid UTF-8";
    return false;
  }

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(payload);
  } catch (const std::exception& ex) {
    error = std::string("JSON parse error: ") + ex.what();
    return false;
  }

  auto msg = message_from_json(j, error);
  if (!msg) return false;
  out = *msg;
  return true;
}

bool read_frame(int fd, std::vector<std::uint8_t>& frame, std::string& error) {
  std::array<std::uint8_t, kFramePrefixBytes> prefix{};
  ssize_t n = read_exact(fd, prefix.data(), prefix.size());
  if (n == 0) {
    error = "EOF";
    return false;
  }
  if (n != static_cast<ssize_t>(prefix.size())) {
    error = "failed to read length prefix";
    return false;
  }
  std::uint32_t be_len = 0;
  std::memcpy(&be_len, prefix.data(), sizeof(be_len));
  const std::uint32_t payload_len = from_be32(be_len);
  if (payload_len > kMaxPayloadSize) {
    error = "payload too large";
    return false;
  }

  frame.resize(kFramePrefixBytes + payload_len);
  std::memcpy(frame.data(), prefix.data(), kFramePrefixBytes);
  if (payload_len == 0) {
    return true;
  }

  ssize_t r = read_exact(fd, frame.data() + kFramePrefixBytes, payload_len);
  if (r != static_cast<ssize_t>(payload_len)) {
    error = "failed to read payload";
    return false;
  }
  return true;
}

bool write_frame(int fd, const std::vector<std::uint8_t>& frame,
                 std::string& error) {
  if (frame.size() < kFramePrefixBytes) {
    error = "frame too small to write";
    return false;
  }
  ssize_t n = write_exact(fd, frame.data(), frame.size());
  if (n != static_cast<ssize_t>(frame.size())) {
    error = "failed to write full frame";
    return false;
  }
  return true;
}

}  // namespace quiz
