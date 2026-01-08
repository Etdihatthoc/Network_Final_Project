#pragma once

#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "common/message.hpp"

namespace quiz::server {

struct RoomSettings {
  int total_questions{0};
  int duration_seconds{0};
  int easy{0};
  int medium{0};
  int hard{0};
};

struct RoomInfo {
  int id{};
  std::string code;
  std::string name;
  std::string description;
  int duration_seconds{};
  std::string status;  // WAITING, IN_PROGRESS, FINISHED
  int creator_id{};
  std::string creator_name;
  int participant_count{0};
  std::uint64_t started_at{0};  // Unix timestamp when room started (0 if not started)
};

struct RoomParticipant {
  int user_id{};
  std::string username;
  std::string full_name;
  std::string status;  // READY, IN_EXAM, SUBMITTED
  std::uint64_t joined_at{};
};

struct RoomDetails {
  RoomInfo info;
  std::string creator_name;
  std::vector<RoomParticipant> participants;
};

struct ExamPaper {
  int exam_id{};
  int room_id{};
  std::vector<nlohmann::json> questions;
  std::uint64_t start_time{};
  std::uint64_t end_time{};
};

struct PracticePaper {
  int practice_id{};
  std::vector<nlohmann::json> questions;
  std::uint64_t start_time{};
  std::uint64_t end_time{};
};

struct RoomResultRow {
  int user_id{};
  std::string username;
  std::string full_name;
  double score{};
  int correct{};
  int total{};
  std::uint64_t submitted_at{};
};

struct RoomResult {
  std::vector<RoomResultRow> rows;
  double average_score{};
  double highest_score{};
  double lowest_score{};
  double pass_rate{};
};

struct UserHistory {
  nlohmann::json exams;       // array
  nlohmann::json practices;  // array
  double avg_score{};
};

class RoomManager {
 public:
  explicit RoomManager(std::string db_path);
  ~RoomManager();

  RoomManager(const RoomManager&) = delete;
  RoomManager& operator=(const RoomManager&) = delete;

  std::optional<RoomInfo> create_room(int creator_id,
                                      const std::string& name,
                                      const std::string& description,
                                      const std::string& room_pass,
                                      const RoomSettings& settings,
                                      std::string* error = nullptr);

  std::vector<RoomInfo> list_rooms(const std::optional<std::string>& status_filter = std::nullopt,
                                   std::string* error = nullptr);

  bool join_room(int room_id, int user_id, const std::string& pass, std::string* error = nullptr);

  bool start_room(int room_id, int creator_id, std::string* error = nullptr);

  std::optional<ExamPaper> get_exam_paper(int room_id, int user_id, std::string* error = nullptr);

  bool submit_answers(int exam_id, const std::vector<std::pair<int, std::string>>& answers,
                      std::string* error = nullptr);

  bool submit_exam(int exam_id, const std::vector<std::pair<int, std::string>>& answers,
                   int& correct, int& total, double& score, std::string* error = nullptr);

  std::optional<PracticePaper> start_practice(int user_id,
                                              int question_count,
                                              int duration_sec,
                                              const std::vector<std::string>& difficulties,
                                              const std::vector<std::string>& topics,
                                              std::string* error = nullptr);

  bool submit_practice(int practice_id,
                       int user_id,
                       const std::vector<std::pair<int, std::string>>& answers,
                       int& correct,
                       int& total,
                       double& score,
                       std::string* error = nullptr);

  std::optional<RoomResult> get_room_results(int room_id, std::string* error = nullptr);

  std::optional<UserHistory> get_user_history(int user_id, std::string* error = nullptr);

  std::optional<RoomDetails> get_room_details(int room_id, std::string* error = nullptr);

  // Delete a room (only creator can delete, not allowed for IN_PROGRESS rooms)
  bool delete_room(int room_id, int user_id, std::string* error = nullptr);

  // Finish a room (only creator can finish, changes status from IN_PROGRESS to FINISHED)
  bool finish_room(int room_id, int user_id, std::string* error = nullptr);

  // Auto-submit expired exams (for background worker)
  int auto_submit_expired_exams(std::string* error = nullptr);

  // Get timer status for an exam (returns remaining seconds, server time, etc.)
  struct TimerStatus {
    std::uint64_t started_at{};     // Unix timestamp when exam started
    std::uint32_t duration_sec{};   // Total duration in seconds
    std::int32_t remaining_sec{};   // Remaining seconds (can be negative if expired)
    std::uint64_t server_time{};    // Current server time (Unix timestamp)
  };
  std::optional<TimerStatus> get_timer_status(int exam_id, std::string* error = nullptr);

  bool exam_owned_by(int exam_id, int user_id);

 private:
  bool open_db();
  std::vector<nlohmann::json> pick_questions(const RoomSettings& settings, std::string* error);
  std::vector<nlohmann::json> pick_questions_filtered(int count,
                                                      const std::vector<std::string>& difficulties,
                                                      const std::vector<std::string>& topics,
                                                      std::string* error);
  int ensure_exam(int room_id, int user_id, std::uint64_t start_time, std::uint64_t end_time, std::string* error);
  bool is_room_waiting(int room_id);
  bool is_participant(int room_id, int user_id);

  // Helper functions for exam questions management
  std::vector<nlohmann::json> load_exam_questions(int exam_id, std::string* error);
  bool save_exam_questions(int exam_id, const std::vector<nlohmann::json>& questions, std::string* error);

  std::string db_path_;
  sqlite3* db_{nullptr};
  std::mt19937 rng_;
  mutable std::recursive_mutex db_mutex_;  // Protect database access from multiple threads
};

}  // namespace quiz::server
