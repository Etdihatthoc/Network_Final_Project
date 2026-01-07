#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace quiz {

enum class MessageType { Request, Response, Notification };
enum class Status { None, Success, Error };

struct Message {
  MessageType type{MessageType::Request};
  std::string action;
  std::uint64_t timestamp{0};
  std::string session_id;
  nlohmann::json data{nlohmann::json::object()};
  Status status{Status::None};
  std::string error_code;
  std::string error_message;
};

std::string to_string(MessageType type);
std::string to_string(Status status);

std::optional<MessageType> message_type_from_string(const std::string& value);
std::optional<Status> status_from_string(const std::string& value);

// Convert Message <-> JSON with validation. On failure, returns std::nullopt and fills error.
std::optional<Message> message_from_json(const nlohmann::json& j, std::string& error);
nlohmann::json message_to_json(const Message& msg);

}  // namespace quiz
