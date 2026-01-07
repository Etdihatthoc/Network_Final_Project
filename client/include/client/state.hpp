#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/message.hpp"

namespace quiz::client {

struct RoomRow {
  int room_id{};
  std::string room_code;
  std::string room_name;
  std::string status;
  int duration_seconds{};
};

struct Question {
  int question_id{};
  std::string text;
  std::string difficulty;
  std::string topic;
  std::vector<std::pair<std::string, std::string>> options;  // id, text
  std::string answer;  // selected answer
};

struct ExamSession {
  int room_id{-1};
  int exam_id{-1};
  std::vector<Question> questions;
};

struct PracticeSession {
  int practice_id{-1};
  std::vector<Question> questions;
};

struct ClientState {
  // Auth
  std::string username;
  std::string password;
  std::string token;
  std::string role;

  // Dashboard
  std::vector<RoomRow> rooms;
  std::string new_room_name = "Demo room";
  std::string new_room_desc = "Test";
  int new_room_duration_min = 30;
  int new_room_q_total = 10;
  int new_room_easy = 4;
  int new_room_medium = 4;
  int new_room_hard = 2;

  // Exam
  ExamSession exam;
  PracticeSession practice;

  // History/results (raw JSON string for display)
  std::string last_history;
  std::string last_results;
  std::string last_errors;
};

}  // namespace quiz::client
