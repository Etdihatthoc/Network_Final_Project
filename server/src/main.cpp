#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <thread>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "server/auth.hpp"
#include "server/server.hpp"
#include "server/room.hpp"
using quiz::Message;
using quiz::MessageType;
using quiz::Status;
using quiz::server::AuthService;
using quiz::server::RoomManager;
using quiz::server::Server;
using quiz::server::RoomSettings;
using quiz::server::RoomResult;

namespace {
std::atomic<bool> g_stop{false};

void signal_handler(int) {
  g_stop.store(true);
}

Message make_error_response(const Message& req,
                            const std::string& code,
                            const std::string& msg) {
  Message resp;
  resp.type = MessageType::Response;
  resp.action = req.action;
  resp.timestamp = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  resp.status = Status::Error;
  resp.error_code = code;
  resp.error_message = msg;
  return resp;
}

Message echo_handler(const Message& req) {
  Message resp;
  resp.type = MessageType::Response;
  resp.action = req.action;
  resp.timestamp = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  resp.status = Status::Success;
  resp.data = req.data;
  return resp;
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "0.0.0.0";
  uint16_t port = 5555;
  std::string db_path = "../data/quiz.db";
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  if (argc > 2) {
    db_path = argv[2];
  }

  Server server(host, port, 4);
  AuthService auth(db_path);
  RoomManager room_mgr(db_path);
  // Ensure logs dir exists and set up rotating logger.
  std::filesystem::create_directories("logs");
  auto logger = spdlog::rotating_logger_mt("server", "logs/server.log", 1024 * 1024 * 5, 3);
  spdlog::set_default_logger(logger);
  spdlog::set_level(spdlog::level::info);
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

  server.register_handler("ECHO", echo_handler);

  std::cout << "[DEBUG] Registering REGISTER handler...\n";
  server.register_handler("REGISTER", [&auth](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.status = Status::Error;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    const auto& d = req.data;
    if (!d.contains("username") || !d.contains("password") || !d.contains("full_name")) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "Missing required fields";
      return resp;
    }
    std::string error;
    auto user_id = auth.register_user(d.value("username", ""), d.value("password", ""),
                                      d.value("full_name", ""), d.value("email", ""),
                                      "STUDENT", &error);
    if (!user_id) {
      resp.error_code = "REGISTER_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"user_id", *user_id}};
    return resp;
  });

  server.register_handler("LOGIN", [&auth](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.status = Status::Error;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    const auto& d = req.data;
    if (!d.contains("username") || !d.contains("password")) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "Missing username/password";
      return resp;
    }
    std::string error;
    auto session = auth.login(d.value("username", ""), d.value("password", ""), 3600, &error);
    if (!session) {
      resp.error_code = "LOGIN_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.session_id = session->token;
    resp.data = {{"user_id", session->user_id},
                 {"username", session->username},
                 {"role", session->role},
                 {"expires_at", session->expires_at},
                 {"session_id", session->token}};
    return resp;
  });

  server.register_handler("LOGOUT", [&auth](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.status = Status::Error;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    std::string token = req.session_id;
    if (token.empty() && req.data.contains("session_id")) {
      token = req.data.value("session_id", "");
    }
    if (token.empty()) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "Missing session_id";
      return resp;
    }
    std::string error;
    if (!auth.logout(token, &error)) {
      resp.error_code = "LOGOUT_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"message", "Logged out"}};
    return resp;
  });

  // CREATE_ROOM
  server.register_handler("CREATE_ROOM", [&auth, &room_mgr](const Message& req) {
    std::cout << "[DEBUG-CR] CREATE_ROOM handler started\n";
    auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.status = Status::Error;
    resp.timestamp = now;

    std::cout << "[DEBUG-CR] Validating session...\n";
    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      std::cout << "[DEBUG-CR] Session invalid\n";
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    std::cout << "[DEBUG-CR] Session valid, user_id=" << session->user_id << "\n";

    const auto& d = req.data;
    if (!d.contains("room_name") || !d.contains("duration_minutes") || !d.contains("question_settings")) {
      std::cout << "[DEBUG-CR] Missing required fields\n";
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "Missing room fields";
      return resp;
    }

    std::cout << "[DEBUG-CR] Parsing settings...\n";
    RoomSettings settings;
    settings.duration_seconds = d.value("duration_minutes", 0) * 60;
    auto qs = d["question_settings"];
    settings.total_questions = qs.value("total_questions", 10);
    auto diff = qs.value("difficulty_distribution", nlohmann::json::object());
    settings.easy = diff.value("easy", 3);
    settings.medium = diff.value("medium", 4);
    settings.hard = diff.value("hard", 3);
    std::cout << "[DEBUG-CR] Settings parsed: duration=" << settings.duration_seconds
              << " total=" << settings.total_questions << "\n";

    std::string room_name = d.value("room_name", "");
    std::string description = d.value("description", "");
    std::string room_pass = d.value("room_pass", "");
    std::cout << "[DEBUG-CR] Room name='" << room_name << "' desc='" << description << "'\n";

    std::cout << "[DEBUG-CR] Calling room_mgr.create_room()...\n";
    auto room = room_mgr.create_room(session->user_id, room_name, description, room_pass, settings, &error);
    std::cout << "[DEBUG-CR] create_room() returned\n";

    if (!room) {
      std::cout << "[DEBUG-CR] Room creation failed: " << error << "\n";
      resp.error_code = "CREATE_FAILED";
      resp.error_message = error;
      return resp;
    }

    std::cout << "[DEBUG-CR] Building response...\n";
    resp.status = Status::Success;
    resp.data = {{"room_id", room->id},
                 {"room_code", room->code},
                 {"status", room->status},
                 {"duration_seconds", room->duration_seconds}};
    std::cout << "[DEBUG-CR] Response built successfully\n";
    return resp;
  });

  // LIST_ROOMS
  server.register_handler("LIST_ROOMS", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    std::optional<std::string> status_filter;
    if (req.data.contains("filter") && req.data["filter"].contains("status")) {
      auto s = req.data["filter"]["status"];
      if (s.is_string()) status_filter = s.get<std::string>();
    }
    auto rooms = room_mgr.list_rooms(status_filter, &error);
    resp.status = Status::Success;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : rooms) {
      arr.push_back({{"room_id", r.id},
                     {"room_code", r.code},
                     {"room_name", r.name},
                     {"status", r.status},
                     {"duration_seconds", r.duration_seconds},
                     {"creator_id", r.creator_id},
                     {"creator_name", r.creator_name},
                     {"participant_count", r.participant_count},
                     {"started_at", r.started_at}});
    }
    resp.data = {{"rooms", arr}};
    return resp;
  });

  // JOIN_ROOM
  server.register_handler("JOIN_ROOM", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int room_id = req.data.value("room_id", -1);
    std::string room_pass = req.data.value("room_pass", "");
    if (room_id <= 0) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "room_id required";
      return resp;
    }
    if (!room_mgr.join_room(room_id, session->user_id, room_pass, &error)) {
      resp.error_code = "JOIN_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"room_id", room_id}, {"user_id", session->user_id}};
    return resp;
  });

  // START_EXAM
  server.register_handler("START_EXAM", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int room_id = req.data.value("room_id", -1);
    if (room_id <= 0) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "room_id required";
      return resp;
    }
    if (!room_mgr.start_room(room_id, session->user_id, &error)) {
      resp.error_code = "START_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"room_id", room_id}, {"status", "IN_PROGRESS"}};
    return resp;
  });

  // GET_EXAM_PAPER
  server.register_handler("GET_EXAM_PAPER", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int room_id = req.data.value("room_id", -1);
    if (room_id <= 0) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "room_id required";
      return resp;
    }
    auto paper = room_mgr.get_exam_paper(room_id, session->user_id, &error);
    if (!paper) {
      resp.error_code = "EXAM_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"exam_id", paper->exam_id},
                 {"room_id", paper->room_id},
                 {"start_time", paper->start_time},
                 {"end_time", paper->end_time},
                 {"questions", paper->questions}};
    return resp;
  });

  // GET_TIMER_STATUS
  server.register_handler("GET_TIMER_STATUS", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int exam_id = req.data.value("exam_id", -1);
    if (exam_id <= 0) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "exam_id required";
      return resp;
    }
    auto timer = room_mgr.get_timer_status(exam_id, &error);
    if (!timer) {
      resp.error_code = "TIMER_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"started_at", timer->started_at},
                 {"duration_sec", timer->duration_sec},
                 {"remaining_sec", timer->remaining_sec},
                 {"server_time", timer->server_time}};
    return resp;
  });

  // SUBMIT_ANSWER (patch)
  server.register_handler("SUBMIT_ANSWER", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int exam_id = req.data.value("exam_id", -1);
    if (exam_id <= 0 || !req.data.contains("answers")) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "exam_id and answers required";
      return resp;
    }
    // Validate answers is an array
    if (!req.data["answers"].is_array()) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "answers must be an array";
      return resp;
    }
    if (!room_mgr.exam_owned_by(exam_id, session->user_id)) {
      resp.error_code = "FORBIDDEN";
      resp.error_message = "exam not owned by user";
      return resp;
    }
    std::vector<std::pair<int, std::string>> answers;
    for (auto& a : req.data["answers"]) {
      int qid = a.value("question_id", -1);
      std::string sel = a.value("selected_option", "");
      if (qid > 0 && !sel.empty()) answers.push_back({qid, sel});
    }
    if (!room_mgr.submit_answers(exam_id, answers, &error)) {
      resp.error_code = "SUBMIT_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"saved_count", static_cast<int>(answers.size())}};
    return resp;
  });

  // SUBMIT_EXAM (final)
  server.register_handler("SUBMIT_EXAM", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int exam_id = req.data.value("exam_id", -1);
    if (exam_id <= 0 || !req.data.contains("final_answers")) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "exam_id and final_answers required";
      return resp;
    }
    // Validate final_answers is an array
    if (!req.data["final_answers"].is_array()) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "final_answers must be an array";
      return resp;
    }
    if (!room_mgr.exam_owned_by(exam_id, session->user_id)) {
      resp.error_code = "FORBIDDEN";
      resp.error_message = "exam not owned by user";
      return resp;
    }
    std::vector<std::pair<int, std::string>> answers;
    for (auto& a : req.data["final_answers"]) {
      int qid = a.value("question_id", -1);
      std::string sel = a.value("selected_option", "");
      if (qid > 0 && !sel.empty()) answers.push_back({qid, sel});
    }
    int correct = 0, total = 0;
    double score = 0.0;
    if (!room_mgr.submit_exam(exam_id, answers, correct, total, score, &error)) {
      resp.error_code = "SUBMIT_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"exam_id", exam_id},
                 {"correct_answers", correct},
                 {"total_questions", total},
                 {"score", score}};
    return resp;
  });

  // START_PRACTICE
  server.register_handler("START_PRACTICE", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int qcount = req.data.value("question_count", 10);
    int duration = req.data.value("duration_minutes", 30) * 60;
    std::vector<std::string> diffs;
    if (req.data.contains("difficulty_filter")) {
      for (auto& d : req.data["difficulty_filter"]) diffs.push_back(d.get<std::string>());
    }
    std::vector<std::string> topics;
    if (req.data.contains("topic_filter")) {
      for (auto& t : req.data["topic_filter"]) topics.push_back(t.get<std::string>());
    }
    auto paper = room_mgr.start_practice(session->user_id, qcount, duration, diffs, topics, &error);
    if (!paper) {
      resp.error_code = "PRACTICE_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"practice_id", paper->practice_id},
                 {"start_time", paper->start_time},
                 {"end_time", paper->end_time},
                 {"questions", paper->questions}};
    return resp;
  });

  // SUBMIT_PRACTICE
  server.register_handler("SUBMIT_PRACTICE", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int pid = req.data.value("practice_id", -1);
    if (pid <= 0 || !req.data.contains("final_answers")) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "practice_id and final_answers required";
      return resp;
    }
    // Validate final_answers is an array
    if (!req.data["final_answers"].is_array()) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "final_answers must be an array";
      return resp;
    }
    std::vector<std::pair<int, std::string>> answers;
    for (auto& a : req.data["final_answers"]) {
      int qid = a.value("question_id", -1);
      std::string sel = a.value("selected_option", "");
      if (qid > 0 && !sel.empty()) answers.push_back({qid, sel});
    }
    int correct = 0, total = 0;
    double score = 0.0;
    if (!room_mgr.submit_practice(pid, session->user_id, answers, correct, total, score, &error)) {
      resp.error_code = "SUBMIT_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"practice_id", pid},
                 {"correct_answers", correct},
                 {"total_questions", total},
                 {"score", score}};
    return resp;
  });

  // GET_ROOM_RESULTS
  server.register_handler("GET_ROOM_RESULTS", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int room_id = req.data.value("room_id", -1);
    if (room_id <= 0) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "room_id required";
      return resp;
    }
    auto res = room_mgr.get_room_results(room_id, &error);
    if (!res) {
      resp.error_code = "RESULT_FAILED";
      resp.error_message = error;
      return resp;
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& r : res->rows) {
      arr.push_back({{"user_id", r.user_id},
                     {"username", r.username},
                     {"full_name", r.full_name},
                     {"score", r.score},
                     {"correct", r.correct},
                     {"total", r.total},
                     {"submitted_at", r.submitted_at}});
    }
    resp.status = Status::Success;
    resp.data = {{"participants", arr},
                 {"statistics",
                  {{"average_score", res->average_score},
                   {"highest_score", res->highest_score},
                   {"lowest_score", res->lowest_score},
                   {"pass_rate", res->pass_rate}}}};
    return resp;
  });

  // GET_ROOM_DETAILS
  server.register_handler("GET_ROOM_DETAILS", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int room_id = req.data.value("room_id", -1);
    if (room_id <= 0) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "room_id required";
      return resp;
    }
    auto details = room_mgr.get_room_details(room_id, &error);
    if (!details) {
      resp.error_code = "DETAILS_FAILED";
      resp.error_message = error;
      return resp;
    }

    // Build participants array
    nlohmann::json participants = nlohmann::json::array();
    for (const auto& p : details->participants) {
      participants.push_back({
        {"user_id", p.user_id},
        {"username", p.username},
        {"full_name", p.full_name},
        {"status", p.status},
        {"joined_at", p.joined_at}
      });
    }

    resp.status = Status::Success;
    resp.data = {
      {"room_id", details->info.id},
      {"room_code", details->info.code},
      {"room_name", details->info.name},
      {"description", details->info.description},
      {"duration_seconds", details->info.duration_seconds},
      {"status", details->info.status},
      {"creator_id", details->info.creator_id},
      {"creator_name", details->creator_name},
      {"participant_count", details->info.participant_count},
      {"participants", participants}
    };
    return resp;
  });

  // DELETE_ROOM
  server.register_handler("DELETE_ROOM", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }

    if (!req.data.contains("room_id") || !req.data["room_id"].is_number()) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "room_id is required and must be a number";
      return resp;
    }

    int room_id = req.data["room_id"].get<int>();

    if (!room_mgr.delete_room(room_id, session->user_id, &error)) {
      resp.error_code = "DELETE_FAILED";
      resp.error_message = error;
      return resp;
    }

    resp.status = Status::Success;
    resp.data = {{"message", "room deleted successfully"}, {"room_id", room_id}};
    return resp;
  });

  // FINISH_ROOM
  server.register_handler("FINISH_ROOM", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }

    if (!req.data.contains("room_id") || !req.data["room_id"].is_number()) {
      resp.error_code = "INVALID_REQUEST";
      resp.error_message = "room_id is required and must be a number";
      return resp;
    }

    int room_id = req.data["room_id"].get<int>();

    if (!room_mgr.finish_room(room_id, session->user_id, &error)) {
      resp.error_code = "FINISH_FAILED";
      resp.error_message = error;
      return resp;
    }

    resp.status = Status::Success;
    resp.data = {{"message", "room finished successfully"}, {"room_id", room_id}};
    return resp;
  });

  // GET_USER_HISTORY
  server.register_handler("GET_USER_HISTORY", [&auth, &room_mgr](const Message& req) {
    Message resp;
    resp.type = MessageType::Response;
    resp.action = req.action;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    resp.status = Status::Error;

    std::string error;
    auto session = auth.validate(req.session_id, &error);
    if (!session) {
      resp.error_code = "UNAUTHORIZED";
      resp.error_message = error;
      return resp;
    }
    int target_user = session->user_id;
    if (req.data.contains("user_id")) {
      target_user = req.data.value("user_id", session->user_id);
    }
    auto hist = room_mgr.get_user_history(target_user, &error);
    if (!hist) {
      resp.error_code = "HISTORY_FAILED";
      resp.error_message = error;
      return resp;
    }
    resp.status = Status::Success;
    resp.data = {{"exams", hist->exams},
                 {"practices", hist->practices},
                 {"average_score", hist->avg_score}};
    return resp;
  });

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  if (!server.start()) {
    std::cerr << "[server] failed to start\n";
    return 1;
  }

  std::cout << "[server] listening on " << host << ":" << port
            << " (handlers: ECHO). Press Ctrl+C to stop.\n";

  // TEMPORARILY DISABLED: Background worker for auto-submitting expired exams
  // This may cause race conditions with database access
  /*
  std::thread auto_submit_worker([&room_mgr]() {
    while (!g_stop.load()) {
      // Sleep for 10 seconds between checks
      for (int i = 0; i < 50 && !g_stop.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      if (g_stop.load()) break;

      // Auto-submit expired exams
      std::string error;
      int count = room_mgr.auto_submit_expired_exams(&error);
      if (count > 0) {
        std::cout << "[worker] Auto-submitted " << count << " expired exam(s)\n";
      } else if (count < 0 && !error.empty()) {
        std::cout << "[worker] Auto-submit failed: " << error << "\n";
      }
    }
  });
  */

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // Wait for worker thread to finish
  /*
  if (auto_submit_worker.joinable()) {
    auto_submit_worker.join();
  }
  */

  server.stop();
  std::cout << "[server] stopped.\n";
  return 0;
}
