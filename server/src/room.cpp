#include "server/room.hpp"

#include <chrono>
#include <iostream>
#include <sstream>

namespace quiz::server {

namespace {
std::uint64_t now_seconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}
}  // namespace

RoomManager::RoomManager(std::string db_path)
    : db_path_(std::move(db_path)), rng_(std::random_device{}()) {
  open_db();
}

RoomManager::~RoomManager() {
  if (db_) sqlite3_close(db_);
}

bool RoomManager::open_db() {
  if (db_) return true;
  if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
    db_ = nullptr;
    return false;
  }
  return true;
}

std::optional<RoomInfo> RoomManager::create_room(int creator_id,
                                                 const std::string& name,
                                                 const std::string& description,
                                                 const std::string& room_pass,
                                                 const RoomSettings& settings,
                                                 std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }

  std::string code = "ROOM-" + std::to_string(now_seconds()) + "-" + std::to_string(creator_id);
  const char* sql = "INSERT INTO rooms(code, name, description, duration_sec, total_questions, easy_count, medium_count, hard_count, status, room_pass, creator_id, scheduled_start, created_at) "
                    "VALUES(?,?,?,?, ?, ?, ?, ?, 'WAITING', ?, ?, NULL, ?);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }

  sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, settings.duration_seconds);
  sqlite3_bind_int(stmt, 5, settings.total_questions);
  sqlite3_bind_int(stmt, 6, settings.easy);
  sqlite3_bind_int(stmt, 7, settings.medium);
  sqlite3_bind_int(stmt, 8, settings.hard);
  sqlite3_bind_text(stmt, 9, room_pass.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 10, creator_id);
  sqlite3_bind_int64(stmt, 11, static_cast<sqlite3_int64>(now_seconds()));

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    if (error) *error = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  int room_id = static_cast<int>(sqlite3_last_insert_rowid(db_));
  sqlite3_finalize(stmt);

  RoomInfo info{room_id, code, name, description, settings.duration_seconds, "WAITING", creator_id, "", 0};
  return info;
}

std::vector<RoomInfo> RoomManager::list_rooms(const std::optional<std::string>& status_filter,
                                              std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);  // Thread-safe database access

  std::vector<RoomInfo> rooms;
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return rooms;
  }
  std::string sql = "SELECT r.id, r.code, r.name, r.description, r.duration_sec, r.status, r.creator_id, "
                    "u.username as creator_name, "
                    "(SELECT COUNT(*) FROM room_participants p WHERE p.room_id = r.id) as pcnt, "
                    "r.started_at "
                    "FROM rooms r "
                    "LEFT JOIN users u ON r.creator_id = u.id";
  if (status_filter && !status_filter->empty()) {
    sql += " WHERE status = ?";
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return rooms;
  }
  if (status_filter && !status_filter->empty()) {
    sqlite3_bind_text(stmt, 1, status_filter->c_str(), -1, SQLITE_TRANSIENT);
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    RoomInfo r;
    r.id = sqlite3_column_int(stmt, 0);

    // Safe string extraction with NULL checks
    const unsigned char* code_ptr = sqlite3_column_text(stmt, 1);
    r.code = code_ptr ? reinterpret_cast<const char*>(code_ptr) : "";

    const unsigned char* name_ptr = sqlite3_column_text(stmt, 2);
    r.name = name_ptr ? reinterpret_cast<const char*>(name_ptr) : "";

    const unsigned char* desc_ptr = sqlite3_column_text(stmt, 3);
    r.description = desc_ptr ? reinterpret_cast<const char*>(desc_ptr) : "";

    r.duration_seconds = sqlite3_column_int(stmt, 4);

    const unsigned char* status_ptr = sqlite3_column_text(stmt, 5);
    r.status = status_ptr ? reinterpret_cast<const char*>(status_ptr) : "";

    r.creator_id = sqlite3_column_int(stmt, 6);

    const unsigned char* creator_ptr = sqlite3_column_text(stmt, 7);
    r.creator_name = creator_ptr ? reinterpret_cast<const char*>(creator_ptr) : "";

    r.participant_count = sqlite3_column_int(stmt, 8);
    r.started_at = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 9));
    rooms.push_back(std::move(r));
  }
  sqlite3_finalize(stmt);
  return rooms;
}

bool RoomManager::join_room(int room_id, int user_id, const std::string& pass, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return false;
  }
  // room must be IN_PROGRESS and pass matched
  const char* chk_sql = "SELECT status, room_pass FROM rooms WHERE id = ?;";
  sqlite3_stmt* chk = nullptr;
  if (sqlite3_prepare_v2(db_, chk_sql, -1, &chk, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  sqlite3_bind_int(chk, 1, room_id);
  if (sqlite3_step(chk) != SQLITE_ROW) {
    if (error) *error = "room not found";
    sqlite3_finalize(chk);
    return false;
  }
  std::string st = reinterpret_cast<const char*>(sqlite3_column_text(chk, 0));
  std::string real_pass = reinterpret_cast<const char*>(sqlite3_column_text(chk, 1) ? sqlite3_column_text(chk, 1) : reinterpret_cast<const unsigned char*>(""));
  sqlite3_finalize(chk);
  // Allow joining WAITING or IN_PROGRESS rooms
  // WAITING: join before exam starts, IN_PROGRESS: join during exam
  if (st != "WAITING" && st != "IN_PROGRESS") {
    if (error) *error = "room has finished or invalid status";
    return false;
  }
  if (real_pass != pass) {
    if (error) *error = "wrong room password";
    return false;
  }

  const char* sql = "INSERT OR IGNORE INTO room_participants(room_id, user_id, status, joined_at) "
                    "VALUES(?, ?, 'READY', ?);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  sqlite3_bind_int(stmt, 1, room_id);
  sqlite3_bind_int(stmt, 2, user_id);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(now_seconds()));
  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  if (!ok && error) *error = sqlite3_errmsg(db_);
  sqlite3_finalize(stmt);
  return ok;
}

bool RoomManager::start_room(int room_id, int creator_id, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return false;
  }
  const char* sql = "UPDATE rooms SET status = 'IN_PROGRESS', started_at = ? WHERE id = ? AND creator_id = ? AND status = 'WAITING';";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(now_seconds()));
  sqlite3_bind_int(stmt, 2, room_id);
  sqlite3_bind_int(stmt, 3, creator_id);
  bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  if (!ok && error) *error = "Cannot start (not creator or not waiting)";
  sqlite3_finalize(stmt);
  return ok;
}

std::vector<nlohmann::json> RoomManager::pick_questions(const RoomSettings& settings, std::string* error) {
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return {};
  }
  auto fetch = [&](const std::string& difficulty, int count) -> std::vector<nlohmann::json> {
    std::vector<nlohmann::json> out;
    if (count <= 0) return out;
    std::string sql = "SELECT id, text, options_json, correct_option, topic, difficulty FROM questions WHERE difficulty = ? ORDER BY RANDOM() LIMIT ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      return out;
    }
    sqlite3_bind_text(stmt, 1, difficulty.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, count);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      nlohmann::json q;
      q["question_id"] = sqlite3_column_int(stmt, 0);
      q["question_text"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
      auto opts_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
      q["options"] = nlohmann::json::parse(opts_raw ? opts_raw : "[]");
      // NOTE: Do NOT include correct_option in response (security fix)
      // correct_option is stored at index 3 but we don't send it to client
      q["topic"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
      q["difficulty"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
      out.push_back(std::move(q));
    }
    sqlite3_finalize(stmt);
    return out;
  };

  std::vector<nlohmann::json> qs;
  auto add = [&](const std::string& d, int c) {
    auto part = fetch(d, c);
    qs.insert(qs.end(), part.begin(), part.end());
  };
  add("EASY", settings.easy);
  add("MEDIUM", settings.medium);
  add("HARD", settings.hard);
  if (settings.total_questions > 0 && static_cast<int>(qs.size()) < settings.total_questions) {
    int missing = settings.total_questions - static_cast<int>(qs.size());
    std::vector<int> ids;
    ids.reserve(qs.size());
    for (const auto& q : qs) {
      ids.push_back(q["question_id"].get<int>());
    }
    std::string sql = "SELECT id, text, options_json, topic, difficulty FROM questions";
    if (!ids.empty()) {
      sql += " WHERE id NOT IN (";
      for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) sql += ",";
        sql += "?";
      }
      sql += ")";
    }
    sql += " ORDER BY RANDOM() LIMIT ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
      int idx = 1;
      for (int id : ids) {
        sqlite3_bind_int(stmt, idx++, id);
      }
      sqlite3_bind_int(stmt, idx, missing);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        nlohmann::json q;
        q["question_id"] = sqlite3_column_int(stmt, 0);
        q["question_text"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        auto opts_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        q["options"] = nlohmann::json::parse(opts_raw ? opts_raw : "[]");
        q["topic"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        q["difficulty"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        qs.push_back(std::move(q));
      }
      sqlite3_finalize(stmt);
    }
  }
  // Trim to total_questions if overshoot
  if (settings.total_questions > 0 && static_cast<int>(qs.size()) > settings.total_questions) {
    std::shuffle(qs.begin(), qs.end(), rng_);
    qs.resize(settings.total_questions);
  }
  return qs;
}

std::vector<nlohmann::json> RoomManager::pick_questions_filtered(int count,
                                                                 const std::vector<std::string>& difficulties,
                                                                 const std::vector<std::string>& topics,
                                                                 std::string* error) {
  std::vector<nlohmann::json> qs;
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return qs;
  }
  std::string sql = "SELECT id, text, options_json, correct_option, topic, difficulty FROM questions";
  std::vector<std::string> clauses;
  if (!difficulties.empty()) {
    std::string placeholders(difficulties.size() ? difficulties.size() * 2 - 1 : 0, '?');
    for (size_t i = 1; i < difficulties.size(); ++i) placeholders[2 * i - 1] = ',';
    clauses.push_back("difficulty IN (" + placeholders + ")");
  }
  if (!topics.empty()) {
    std::string placeholders(topics.size() ? topics.size() * 2 - 1 : 0, '?');
    for (size_t i = 1; i < topics.size(); ++i) placeholders[2 * i - 1] = ',';
    clauses.push_back("topic IN (" + placeholders + ")");
  }
  if (!clauses.empty()) {
    sql += " WHERE " + clauses[0];
    for (size_t i = 1; i < clauses.size(); ++i) sql += " AND " + clauses[i];
  }
  sql += " ORDER BY RANDOM() LIMIT ?";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return qs;
  }
  int idx = 1;
  for (const auto& d : difficulties) sqlite3_bind_text(stmt, idx++, d.c_str(), -1, SQLITE_TRANSIENT);
  for (const auto& t : topics) sqlite3_bind_text(stmt, idx++, t.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, idx, count);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    nlohmann::json q;
    q["question_id"] = sqlite3_column_int(stmt, 0);
    q["question_text"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    auto opts_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    q["options"] = nlohmann::json::parse(opts_raw ? opts_raw : "[]");
    // NOTE: Do NOT include correct_option in response (security fix)
    // correct_option is at index 3 but we don't send it to client
    q["topic"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    q["difficulty"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    qs.push_back(std::move(q));
  }
  sqlite3_finalize(stmt);
  return qs;
}

int RoomManager::ensure_exam(int room_id, int user_id, std::uint64_t start_time, std::uint64_t end_time, std::string* error) {
  const char* find_sql = "SELECT id FROM exams WHERE room_id = ? AND user_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, find_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return -1;
  }
  sqlite3_bind_int(stmt, 1, room_id);
  sqlite3_bind_int(stmt, 2, user_id);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    int id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
  }
  sqlite3_finalize(stmt);

  const char* ins_sql = "INSERT INTO exams(room_id, user_id, start_at, end_at, total_questions) VALUES(?,?,?,?,0);";
  if (sqlite3_prepare_v2(db_, ins_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return -1;
  }
  sqlite3_bind_int(stmt, 1, room_id);
  sqlite3_bind_int(stmt, 2, user_id);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(start_time));
  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(end_time));
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    if (error) *error = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return -1;
  }
  int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
  sqlite3_finalize(stmt);
  return id;
}

// Helper: Load existing exam questions from exam_questions table
std::vector<nlohmann::json> RoomManager::load_exam_questions(int exam_id, std::string* error) {
  std::vector<nlohmann::json> questions;
  const char* sql = "SELECT q.id, q.text, q.options_json, q.difficulty, q.topic "
                    "FROM exam_questions eq "
                    "JOIN questions q ON eq.question_id = q.id "
                    "WHERE eq.exam_id = ? "
                    "ORDER BY eq.question_order;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return questions;
  }
  sqlite3_bind_int(stmt, 1, exam_id);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    nlohmann::json q;
    q["question_id"] = sqlite3_column_int(stmt, 0);
    q["question_text"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    std::string opts_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    q["options"] = nlohmann::json::parse(opts_raw);
    // NOTE: Do NOT include correct_option in response (security fix)
    q["difficulty"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    q["topic"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    questions.push_back(std::move(q));
  }
  sqlite3_finalize(stmt);
  return questions;
}

// Helper: Save exam questions to exam_questions table
bool RoomManager::save_exam_questions(int exam_id, const std::vector<nlohmann::json>& questions, std::string* error) {
  const char* sql = "INSERT INTO exam_questions(exam_id, question_id, question_order) VALUES(?, ?, ?);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }

  for (size_t i = 0; i < questions.size(); ++i) {
    sqlite3_bind_int(stmt, 1, exam_id);
    sqlite3_bind_int(stmt, 2, questions[i]["question_id"].get<int>());
    sqlite3_bind_int(stmt, 3, static_cast<int>(i + 1));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      if (error) *error = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      return false;
    }
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);
  return true;
}

std::optional<ExamPaper> RoomManager::get_exam_paper(int room_id, int user_id, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }
  // fetch room info
  const char* room_sql = "SELECT duration_sec, status, total_questions, easy_count, medium_count, hard_count FROM rooms WHERE id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, room_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }
  sqlite3_bind_int(stmt, 1, room_id);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    if (error) *error = "room not found";
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  int duration_sec = sqlite3_column_int(stmt, 0);
  std::string status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  int total_questions = sqlite3_column_int(stmt, 2);
  int easy = sqlite3_column_int(stmt, 3);
  int medium = sqlite3_column_int(stmt, 4);
  int hard = sqlite3_column_int(stmt, 5);
  sqlite3_finalize(stmt);

  // Only allow getting exam paper when room is IN_PROGRESS
  if (status != "IN_PROGRESS") {
    if (error) *error = "room not started yet";
    return std::nullopt;
  }
  if (!is_participant(room_id, user_id)) {
    if (error) *error = "not joined this room";
    return std::nullopt;
  }

  // Ensure exam exists (or get existing exam_id)
  std::uint64_t start = now_seconds();
  std::uint64_t end = start + duration_sec;
  int exam_id = ensure_exam(room_id, user_id, start, end, error);
  if (exam_id < 0) return std::nullopt;

  // Use a transaction to prevent race condition when multiple GET_EXAM_PAPER calls happen simultaneously
  // BEGIN IMMEDIATE acquires a write lock immediately
  char* err_msg = nullptr;
  if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    if (error) *error = std::string("Transaction begin failed: ") + (err_msg ? err_msg : "");
    if (err_msg) sqlite3_free(err_msg);
    return std::nullopt;
  }

  // Load existing exam questions if present
  auto questions = load_exam_questions(exam_id, error);
  if (!questions.empty()) {
    // Already got the paper before - prevent getting it again
    if (error) *error = "You have already retrieved the exam paper. Cannot get it twice.";
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return std::nullopt;
  }

  // First time: pick new questions and persist
  RoomSettings settings;
  settings.duration_seconds = duration_sec;
  settings.total_questions = total_questions;
  settings.easy = easy;
  settings.medium = medium;
  settings.hard = hard;
  if (settings.total_questions <= 0) {
    settings.total_questions = settings.easy + settings.medium + settings.hard;
  }
  if (settings.total_questions <= 0) {
    settings.total_questions = 10;
    settings.easy = 4;
    settings.medium = 4;
    settings.hard = 2;
  }

  questions = pick_questions(settings, error);
  if (questions.empty()) {
    if (error && error->empty()) *error = "no questions";
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return std::nullopt;
  }
  if (!save_exam_questions(exam_id, questions, error)) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return std::nullopt;
  }
  // Update total questions count
  const char* upd = "UPDATE exams SET total_questions = ? WHERE id = ?;";
  if (sqlite3_prepare_v2(db_, upd, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, static_cast<int>(questions.size()));
    sqlite3_bind_int(stmt, 2, exam_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  // Commit the transaction
  if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &err_msg) != SQLITE_OK) {
    if (error) *error = std::string("Transaction commit failed: ") + (err_msg ? err_msg : "");
    if (err_msg) sqlite3_free(err_msg);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return std::nullopt;
  }

  ExamPaper paper;
  paper.exam_id = exam_id;
  paper.room_id = room_id;
  paper.questions = std::move(questions);
  paper.start_time = start;
  paper.end_time = end;
  return paper;
}

bool RoomManager::submit_answers(int exam_id, const std::vector<std::pair<int, std::string>>& answers,
                                 std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return false;
  }
  const char* sql = "INSERT INTO answers(exam_id, question_id, selected_option, updated_at) "
                    "VALUES(?,?,?, ?) "
                    "ON CONFLICT(exam_id, question_id) DO UPDATE SET selected_option=excluded.selected_option, updated_at=excluded.updated_at;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  for (const auto& ans : answers) {
    sqlite3_reset(stmt);
    sqlite3_bind_int(stmt, 1, exam_id);
    sqlite3_bind_int(stmt, 2, ans.first);
    sqlite3_bind_text(stmt, 3, ans.second.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(now_seconds()));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      if (error) *error = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      return false;
    }
  }
  sqlite3_finalize(stmt);
  return true;
}

bool RoomManager::submit_exam(int exam_id, const std::vector<std::pair<int, std::string>>& answers,
                              int& correct, int& total, double& score, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) return false;

  // FIX: Check if exam already submitted
  const char* check_sql = "SELECT submitted_at FROM exams WHERE id = ?;";
  sqlite3_stmt* check_stmt = nullptr;
  if (sqlite3_prepare_v2(db_, check_sql, -1, &check_stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  sqlite3_bind_int(check_stmt, 1, exam_id);
  if (sqlite3_step(check_stmt) == SQLITE_ROW) {
    // Check if submitted_at is NOT NULL
    if (sqlite3_column_type(check_stmt, 0) != SQLITE_NULL) {
      if (error) *error = "Exam already submitted";
      sqlite3_finalize(check_stmt);
      return false;
    }
  } else {
    if (error) *error = "Exam not found";
    sqlite3_finalize(check_stmt);
    return false;
  }
  sqlite3_finalize(check_stmt);

  if (!submit_answers(exam_id, answers, error)) return false;

  // Fetch correct answers for grading
  const char* sql = "SELECT a.question_id, a.selected_option, q.correct_option "
                    "FROM answers a JOIN questions q ON a.question_id = q.id "
                    "WHERE a.exam_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  sqlite3_bind_int(stmt, 1, exam_id);
  correct = 0;
  total = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* sel = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const char* right = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    if (sel && right && std::string(sel) == std::string(right)) {
      ++correct;
    }
    ++total;
  }
  sqlite3_finalize(stmt);
  if (total == 0) total = static_cast<int>(answers.size());
  score = total > 0 ? static_cast<double>(correct) * 10.0 / total : 0.0;

  // Update with WHERE submitted_at IS NULL for extra safety
  const char* upd = "UPDATE exams SET submitted_at = ?, correct_count = ?, score = ?, total_questions = ? WHERE id = ? AND submitted_at IS NULL;";
  if (sqlite3_prepare_v2(db_, upd, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(now_seconds()));
  sqlite3_bind_int(stmt, 2, correct);
  sqlite3_bind_double(stmt, 3, score);
  sqlite3_bind_int(stmt, 4, total);
  sqlite3_bind_int(stmt, 5, exam_id);
  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  if (!ok && error) *error = sqlite3_errmsg(db_);
  sqlite3_finalize(stmt);

  // Check if update actually happened
  if (ok && sqlite3_changes(db_) == 0) {
    if (error) *error = "Exam already submitted or not found";
    return false;
  }
  return ok;
}

bool RoomManager::is_room_waiting(int room_id) {
  const char* sql = "SELECT status FROM rooms WHERE id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(stmt, 1, room_id);
  bool waiting = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    std::string st = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    waiting = (st == "WAITING");
  }
  sqlite3_finalize(stmt);
  return waiting;
}

bool RoomManager::is_participant(int room_id, int user_id) {
  const char* sql = "SELECT 1 FROM room_participants WHERE room_id = ? AND user_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(stmt, 1, room_id);
  sqlite3_bind_int(stmt, 2, user_id);
  bool ok = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return ok;
}

bool RoomManager::exam_owned_by(int exam_id, int user_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  const char* sql = "SELECT 1 FROM exams WHERE id = ? AND user_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(stmt, 1, exam_id);
  sqlite3_bind_int(stmt, 2, user_id);
  bool ok = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return ok;
}

std::optional<PracticePaper> RoomManager::start_practice(int user_id,
                                                         int question_count,
                                                         int duration_sec,
                                                         const std::vector<std::string>& difficulties,
                                                         const std::vector<std::string>& topics,
                                                         std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }
  auto qs = pick_questions_filtered(question_count, difficulties, topics, error);
  if (qs.empty()) {
    if (error && error->empty()) *error = "no questions";
  }
  std::uint64_t start = now_seconds();
  std::uint64_t end = start + duration_sec;
  nlohmann::json settings = {{"question_count", question_count},
                             {"duration_sec", duration_sec},
                             {"difficulties", difficulties},
                             {"topics", topics}};
  const char* sql = "INSERT INTO practice_runs(user_id, start_at, end_at, total_questions, settings_json) "
                    "VALUES(?,?,?,?,?);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }
  sqlite3_bind_int(stmt, 1, user_id);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(start));
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(end));
  sqlite3_bind_int(stmt, 4, static_cast<int>(qs.size()));
  auto s = settings.dump();
  sqlite3_bind_text(stmt, 5, s.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    if (error) *error = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return std::nullopt;
  }
  int pid = static_cast<int>(sqlite3_last_insert_rowid(db_));
  sqlite3_finalize(stmt);

  PracticePaper paper{pid, qs, start, end};
  return paper;
}

bool RoomManager::submit_practice(int practice_id,
                                  int user_id,
                                  const std::vector<std::pair<int, std::string>>& answers,
                                  int& correct,
                                  int& total,
                                  double& score,
                                  std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return false;
  }
  // Grade using questions table
  correct = 0;
  total = static_cast<int>(answers.size());
  for (const auto& a : answers) {
    const char* sql = "SELECT correct_option FROM questions WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      if (error) *error = sqlite3_errmsg(db_);
      return false;
    }
    sqlite3_bind_int(stmt, 1, a.first);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char* right = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
      if (right && a.second == std::string(right)) ++correct;
    }
    sqlite3_finalize(stmt);
  }
  score = total > 0 ? static_cast<double>(correct) * 10.0 / total : 0.0;

  const char* upd = "UPDATE practice_runs SET correct_count = ?, score = ?, end_at = ? WHERE id = ? AND user_id = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, upd, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }
  sqlite3_bind_int(stmt, 1, correct);
  sqlite3_bind_double(stmt, 2, score);
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(now_seconds()));
  sqlite3_bind_int(stmt, 4, practice_id);
  sqlite3_bind_int(stmt, 5, user_id);
  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  if (!ok && error) *error = sqlite3_errmsg(db_);
  sqlite3_finalize(stmt);
  return ok;
}

std::optional<RoomResult> RoomManager::get_room_results(int room_id, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  RoomResult result;
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }
  const char* sql = "SELECT u.id, u.username, u.full_name, e.score, e.correct_count, e.total_questions, e.submitted_at "
                    "FROM exams e JOIN users u ON e.user_id = u.id "
                    "WHERE e.room_id = ? AND e.submitted_at IS NOT NULL;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }
  sqlite3_bind_int(stmt, 1, room_id);
  double sum = 0.0;
  double hi = -1e9, lo = 1e9;
  int count = 0, pass = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    RoomResultRow row;
    row.user_id = sqlite3_column_int(stmt, 0);
    row.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    row.full_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    row.score = sqlite3_column_double(stmt, 3);
    row.correct = sqlite3_column_int(stmt, 4);
    row.total = sqlite3_column_int(stmt, 5);
    row.submitted_at = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 6));
    result.rows.push_back(row);
    sum += row.score;
    if (row.score > hi) hi = row.score;
    if (row.score < lo) lo = row.score;
    if (row.score >= 5.0) ++pass;
    ++count;
  }
  sqlite3_finalize(stmt);
  if (count > 0) {
    result.average_score = sum / count;
    result.highest_score = hi;
    result.lowest_score = lo;
    result.pass_rate = (static_cast<double>(pass) * 100.0) / count;
  }
  return result;
}

std::optional<UserHistory> RoomManager::get_user_history(int user_id, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  UserHistory hist;
  hist.exams = nlohmann::json::array();
  hist.practices = nlohmann::json::array();
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }
  // Exams
  const char* ex_sql = "SELECT e.id, e.room_id, e.score, e.correct_count, e.total_questions, e.submitted_at, r.name "
                       "FROM exams e LEFT JOIN rooms r ON e.room_id = r.id "
                       "WHERE e.user_id = ? AND e.submitted_at IS NOT NULL;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, ex_sql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, user_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      nlohmann::json item;
      item["exam_id"] = sqlite3_column_int(stmt, 0);
      item["room_id"] = sqlite3_column_int(stmt, 1);
      item["score"] = sqlite3_column_double(stmt, 2);
      item["correct"] = sqlite3_column_int(stmt, 3);
      item["total"] = sqlite3_column_int(stmt, 4);
      item["submitted_at"] = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 5));
      const char* rn = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
      if (rn) item["room_name"] = rn;
      hist.exams.push_back(item);
    }
    sqlite3_finalize(stmt);
  }
  // Practices
  const char* pr_sql = "SELECT id, score, correct_count, total_questions, end_at, settings_json "
                       "FROM practice_runs WHERE user_id = ?;";
  if (sqlite3_prepare_v2(db_, pr_sql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int(stmt, 1, user_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      nlohmann::json item;
      item["practice_id"] = sqlite3_column_int(stmt, 0);
      item["score"] = sqlite3_column_double(stmt, 1);
      item["correct"] = sqlite3_column_int(stmt, 2);
      item["total"] = sqlite3_column_int(stmt, 3);
      item["submitted_at"] = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 4));
      const char* sj = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
      if (sj) item["settings"] = nlohmann::json::parse(sj);
      hist.practices.push_back(item);
    }
    sqlite3_finalize(stmt);
  }
  // Average score
  double sum = 0.0;
  int cnt = 0;
  for (auto& e : hist.exams) { sum += e.value("score", 0.0); ++cnt; }
  for (auto& p : hist.practices) { sum += p.value("score", 0.0); ++cnt; }
  if (cnt > 0) hist.avg_score = sum / cnt;
  return hist;
}

std::optional<RoomDetails> RoomManager::get_room_details(int room_id, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }

  RoomDetails details;

  // Get room info with creator name
  const char* room_sql =
      "SELECT r.id, r.code, r.name, r.description, r.duration_sec, r.status, r.creator_id, "
      "       u.username as creator_name, "
      "       (SELECT COUNT(*) FROM room_participants WHERE room_id = r.id) as participant_count "
      "FROM rooms r "
      "LEFT JOIN users u ON r.creator_id = u.id "
      "WHERE r.id = ?;";

  sqlite3_stmt* room_stmt = nullptr;
  if (sqlite3_prepare_v2(db_, room_sql, -1, &room_stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }

  sqlite3_bind_int(room_stmt, 1, room_id);

  if (sqlite3_step(room_stmt) != SQLITE_ROW) {
    if (error) *error = "Room not found";
    sqlite3_finalize(room_stmt);
    return std::nullopt;
  }

  // Fill room info
  details.info.id = sqlite3_column_int(room_stmt, 0);
  details.info.code = reinterpret_cast<const char*>(sqlite3_column_text(room_stmt, 1));
  details.info.name = reinterpret_cast<const char*>(sqlite3_column_text(room_stmt, 2));
  details.info.description = reinterpret_cast<const char*>(sqlite3_column_text(room_stmt, 3)
                                                           ? sqlite3_column_text(room_stmt, 3)
                                                           : reinterpret_cast<const unsigned char*>(""));
  details.info.duration_seconds = sqlite3_column_int(room_stmt, 4);
  details.info.status = reinterpret_cast<const char*>(sqlite3_column_text(room_stmt, 5));
  details.info.creator_id = sqlite3_column_int(room_stmt, 6);
  details.creator_name = reinterpret_cast<const char*>(sqlite3_column_text(room_stmt, 7)
                                                       ? sqlite3_column_text(room_stmt, 7)
                                                       : reinterpret_cast<const unsigned char*>(""));
  details.info.participant_count = sqlite3_column_int(room_stmt, 8);

  sqlite3_finalize(room_stmt);

  // Get participants list
  const char* part_sql =
      "SELECT rp.user_id, u.username, u.full_name, rp.status, rp.joined_at "
      "FROM room_participants rp "
      "LEFT JOIN users u ON rp.user_id = u.id "
      "WHERE rp.room_id = ? "
      "ORDER BY rp.joined_at ASC;";

  sqlite3_stmt* part_stmt = nullptr;
  if (sqlite3_prepare_v2(db_, part_sql, -1, &part_stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return std::nullopt;
  }

  sqlite3_bind_int(part_stmt, 1, room_id);

  while (sqlite3_step(part_stmt) == SQLITE_ROW) {
    RoomParticipant p;
    p.user_id = sqlite3_column_int(part_stmt, 0);
    p.username = reinterpret_cast<const char*>(sqlite3_column_text(part_stmt, 1)
                                               ? sqlite3_column_text(part_stmt, 1)
                                               : reinterpret_cast<const unsigned char*>(""));
    p.full_name = reinterpret_cast<const char*>(sqlite3_column_text(part_stmt, 2)
                                                ? sqlite3_column_text(part_stmt, 2)
                                                : reinterpret_cast<const unsigned char*>(""));
    p.status = reinterpret_cast<const char*>(sqlite3_column_text(part_stmt, 3));
    p.joined_at = static_cast<std::uint64_t>(sqlite3_column_int64(part_stmt, 4));
    details.participants.push_back(p);
  }

  sqlite3_finalize(part_stmt);
  return details;
}

int RoomManager::auto_submit_expired_exams(std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return -1;
  }

  std::uint64_t now = now_seconds();
  int submitted_count = 0;

  // Find all expired exams that haven't been submitted
  const char* find_sql =
      "SELECT id FROM exams "
      "WHERE end_at < ? AND submitted_at IS NULL;";

  sqlite3_stmt* find_stmt = nullptr;
  if (sqlite3_prepare_v2(db_, find_sql, -1, &find_stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return -1;
  }

  sqlite3_bind_int64(find_stmt, 1, static_cast<sqlite3_int64>(now));

  std::vector<int> expired_exam_ids;
  while (sqlite3_step(find_stmt) == SQLITE_ROW) {
    expired_exam_ids.push_back(sqlite3_column_int(find_stmt, 0));
  }
  sqlite3_finalize(find_stmt);

  // Process each expired exam
  for (int exam_id : expired_exam_ids) {
    // Grade the exam based on submitted answers
    const char* grade_sql =
        "SELECT a.question_id, a.selected_option, q.correct_option "
        "FROM answers a "
        "JOIN questions q ON a.question_id = q.id "
        "WHERE a.exam_id = ?;";

    sqlite3_stmt* grade_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, grade_sql, -1, &grade_stmt, nullptr) != SQLITE_OK) {
      continue; // Skip this exam on error
    }

    sqlite3_bind_int(grade_stmt, 1, exam_id);

    int correct = 0;
    int total = 0;
    while (sqlite3_step(grade_stmt) == SQLITE_ROW) {
      std::string selected = reinterpret_cast<const char*>(sqlite3_column_text(grade_stmt, 1));
      std::string correct_ans = reinterpret_cast<const char*>(sqlite3_column_text(grade_stmt, 2));
      ++total;
      if (selected == correct_ans) ++correct;
    }
    sqlite3_finalize(grade_stmt);

    // If no answers submitted, total might be 0. Get total_questions from exam record
    if (total == 0) {
      const char* count_sql = "SELECT total_questions FROM exams WHERE id = ?;";
      sqlite3_stmt* count_stmt = nullptr;
      if (sqlite3_prepare_v2(db_, count_sql, -1, &count_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(count_stmt, 1, exam_id);
        if (sqlite3_step(count_stmt) == SQLITE_ROW) {
          total = sqlite3_column_int(count_stmt, 0);
        }
        sqlite3_finalize(count_stmt);
      }
    }

    double score = (total > 0) ? (static_cast<double>(correct) / total) * 10.0 : 0.0;

    // Update exam with final score and submitted_at
    // Use WHERE clause to check submitted_at IS NULL to prevent double submission
    const char* update_sql =
        "UPDATE exams SET score = ?, correct_count = ?, total_questions = ?, submitted_at = ? "
        "WHERE id = ? AND submitted_at IS NULL;";

    sqlite3_stmt* update_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, update_sql, -1, &update_stmt, nullptr) != SQLITE_OK) {
      continue; // Skip this exam on error
    }

    sqlite3_bind_double(update_stmt, 1, score);
    sqlite3_bind_int(update_stmt, 2, correct);
    sqlite3_bind_int(update_stmt, 3, total);
    sqlite3_bind_int64(update_stmt, 4, static_cast<sqlite3_int64>(now));
    sqlite3_bind_int(update_stmt, 5, exam_id);

    if (sqlite3_step(update_stmt) == SQLITE_DONE) {
      if (sqlite3_changes(db_) > 0) {
        ++submitted_count;
      }
    }
    sqlite3_finalize(update_stmt);
  }

  return submitted_count;
}

std::optional<RoomManager::TimerStatus> RoomManager::get_timer_status(int exam_id, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);  // Thread-safe database access

  if (!open_db()) {
    if (error) *error = "DB open failed";
    return std::nullopt;
  }

  // Validate exam_id first
  if (exam_id <= 0) {
    if (error) *error = "invalid exam_id";
    return std::nullopt;
  }

  // Get exam info with room details
  const char* sql =
      "SELECT e.start_at, r.started_at, r.duration_sec "
      "FROM exams e "
      "JOIN rooms r ON e.room_id = r.id "
      "WHERE e.id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    // Copy error message immediately before it gets invalidated
    if (error) {
      const char* err_msg = sqlite3_errmsg(db_);
      *error = std::string(err_msg ? err_msg : "prepare failed");
    }
    return std::nullopt;
  }

  sqlite3_bind_int(stmt, 1, exam_id);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    if (error) *error = "exam not found";
    return std::nullopt;
  }

  // Safely extract values with bounds checking
  std::uint64_t exam_start = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
  std::uint64_t room_started = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 1));
  std::uint32_t duration = static_cast<std::uint32_t>(sqlite3_column_int(stmt, 2));

  sqlite3_finalize(stmt);

  // IMPORTANT: Use exam's start_at (individual student timer)
  // Each student gets their own timer starting from when they get the paper
  std::uint64_t started_at = exam_start;
  std::uint64_t current_time = now_seconds();
  std::uint64_t elapsed = (current_time > started_at) ? (current_time - started_at) : 0;
  std::int32_t remaining = static_cast<std::int32_t>(duration) - static_cast<std::int32_t>(elapsed);

  TimerStatus status;
  status.started_at = started_at;
  status.duration_sec = duration;
  status.remaining_sec = remaining;
  status.server_time = current_time;

  return status;
}

bool RoomManager::delete_room(int room_id, int user_id, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return false;
  }

  // Check if room exists and get creator_id and status
  const char* check_sql = "SELECT creator_id, status FROM rooms WHERE id = ?;";
  sqlite3_stmt* stmt = nullptr;

  if (sqlite3_prepare_v2(db_, check_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }

  sqlite3_bind_int(stmt, 1, room_id);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    if (error) *error = "room not found";
    return false;
  }

  int creator_id = sqlite3_column_int(stmt, 0);
  std::string status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  sqlite3_finalize(stmt);

  // Check permissions - only creator can delete
  if (creator_id != user_id) {
    if (error) *error = "only room creator can delete this room";
    return false;
  }

  // Cannot delete IN_PROGRESS rooms
  if (status == "IN_PROGRESS") {
    if (error) *error = "cannot delete room that is in progress";
    return false;
  }

  // Delete the room (cascading deletes will handle related records)
  const char* delete_sql = "DELETE FROM rooms WHERE id = ?;";
  sqlite3_stmt* del_stmt = nullptr;

  if (sqlite3_prepare_v2(db_, delete_sql, -1, &del_stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }

  sqlite3_bind_int(del_stmt, 1, room_id);
  bool success = sqlite3_step(del_stmt) == SQLITE_DONE;

  if (!success && error) {
    *error = sqlite3_errmsg(db_);
  }

  sqlite3_finalize(del_stmt);
  return success;
}

bool RoomManager::finish_room(int room_id, int user_id, std::string* error) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  if (!open_db()) {
    if (error) *error = "DB open failed";
    return false;
  }

  // Check if room exists and get creator_id and status
  const char* check_sql = "SELECT creator_id, status FROM rooms WHERE id = ?;";
  sqlite3_stmt* stmt = nullptr;

  if (sqlite3_prepare_v2(db_, check_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }

  sqlite3_bind_int(stmt, 1, room_id);

  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    if (error) *error = "room not found";
    return false;
  }

  int creator_id = sqlite3_column_int(stmt, 0);
  std::string status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  sqlite3_finalize(stmt);

  // Check permissions - only creator can finish
  if (creator_id != user_id) {
    if (error) *error = "only room creator can finish this room";
    return false;
  }

  // Can only finish IN_PROGRESS rooms
  if (status != "IN_PROGRESS") {
    if (error) *error = "can only finish rooms that are in progress";
    return false;
  }

  // Update room status to FINISHED
  const char* update_sql = "UPDATE rooms SET status = 'FINISHED' WHERE id = ?;";
  sqlite3_stmt* upd_stmt = nullptr;

  if (sqlite3_prepare_v2(db_, update_sql, -1, &upd_stmt, nullptr) != SQLITE_OK) {
    if (error) *error = sqlite3_errmsg(db_);
    return false;
  }

  sqlite3_bind_int(upd_stmt, 1, room_id);
  bool success = sqlite3_step(upd_stmt) == SQLITE_DONE;

  if (!success && error) {
    *error = sqlite3_errmsg(db_);
  }

  sqlite3_finalize(upd_stmt);
  return success;
}

}  // namespace quiz::server
