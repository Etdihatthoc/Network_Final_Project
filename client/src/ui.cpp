#include "client/ui.hpp"

#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <iostream>
#include <sstream>
#include <vector>

#include "common/codec.hpp"
#include "common/message.hpp"

namespace quiz::client {
namespace {

std::string dump_json(const nlohmann::json& j) {
  std::ostringstream oss;
  oss << j.dump(2);
  return oss.str();
}

// -- Event handling ---------------------------------------------------------
void handle_event(ClientState& state, const ClientEvent& ev) {
  const auto& m = ev.message;
  if (m.status == Status::Error) {
    state.last_errors = m.error_code + ": " + m.error_message;
    return;
  }
  if (m.action == "LOGIN") {
    state.token = m.session_id;
    state.role = m.data.value("role", "");
    state.last_errors.clear();
  } else if (m.action == "LIST_ROOMS") {
    state.rooms.clear();
    for (auto& r : m.data["rooms"]) {
      RoomRow row;
      row.room_id = r.value("room_id", -1);
      row.room_code = r.value("room_code", "");
      row.room_name = r.value("room_name", "");
      row.status = r.value("status", "");
      row.duration_seconds = r.value("duration_seconds", 0);
      state.rooms.push_back(row);
    }
    state.last_errors.clear();
  } else if (m.action == "GET_EXAM_PAPER") {
    state.exam.exam_id = m.data.value("exam_id", -1);
    state.exam.room_id = m.data.value("room_id", -1);
    state.exam.questions.clear();
    for (auto& q : m.data["questions"]) {
      Question qu;
      qu.question_id = q.value("question_id", -1);
      qu.text = q.value("question_text", "");
      qu.difficulty = q.value("difficulty", "");
      qu.topic = q.value("topic", "");
      for (auto& [key, val] : q["options"].items()) {
        qu.options.push_back({key, val});
      }
      state.exam.questions.push_back(qu);
    }
    state.last_errors.clear();
  } else if (m.action == "START_PRACTICE") {
    state.practice.practice_id = m.data.value("practice_id", -1);
    state.practice.questions.clear();
    for (auto& q : m.data["questions"]) {
      Question qu;
      qu.question_id = q.value("question_id", -1);
      qu.text = q.value("question_text", "");
      qu.difficulty = q.value("difficulty", "");
      qu.topic = q.value("topic", "");
      for (auto& [key, val] : q["options"].items()) {
        qu.options.push_back({key, val});
      }
      state.practice.questions.push_back(qu);
    }
    state.last_errors.clear();
  } else if (m.action == "SUBMIT_EXAM" || m.action == "SUBMIT_PRACTICE" ||
             m.action == "GET_ROOM_RESULTS" || m.action == "GET_USER_HISTORY") {
    state.last_results = dump_json(m.data);
    state.last_errors.clear();
  }
}

// -- Networking helper -----------------------------------------------------
bool send_request(ClientCore& core, ClientState& state, const std::string& action,
                  const nlohmann::json& data) {
  Message msg;
  msg.type = MessageType::Request;
  msg.action = action;
  msg.timestamp = 0;
  msg.session_id = state.token;
  msg.data = data;
  std::string err;
  if (!core.send_message(msg, err)) {
    state.last_errors = err;
    return false;
  }
  return true;
}

// -- UI subpanels ----------------------------------------------------------
void render_header(ClientState& st, ClientCore& core) {
  static char host[64] = "127.0.0.1";
  static int port = 5555;
  static char user[64] = "teacher";
  static char pass[64] = "teacher123";

  if (ImGui::BeginMainMenuBar()) {
    ImGui::Text("Server");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::InputText("##host", host, IM_ARRAYSIZE(host));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("##port", &port);
    ImGui::SameLine();
    if (ImGui::Button("Connect")) {
      if (core.connect(host, static_cast<uint16_t>(port))) st.last_errors = "Connected";
      else st.last_errors = "Connect failed";
    }
    ImGui::Separator();
    ImGui::Text("Auth");
    ImGui::SameLine();
    ImGui::InputText("##user", user, IM_ARRAYSIZE(user));
    ImGui::SameLine();
    ImGui::InputText("##pass", pass, IM_ARRAYSIZE(pass), ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    if (ImGui::Button("Login")) {
      st.username = user;
      st.password = pass;
      send_request(core, st, "LOGIN", {{"username", st.username}, {"password", st.password}});
    }
    ImGui::SameLine();
    ImGui::Text("Role: %s", st.role.c_str());
    ImGui::SameLine();
    if (!st.token.empty()) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "Token: %.8s...", st.token.c_str());
    ImGui::EndMainMenuBar();
  }
}

void render_rooms(ClientState& st, ClientCore& core) {
  ImGui::Begin("Rooms / Lobby");
  ImGui::Text("Create room");
  ImGui::Separator();
  ImGui::InputText("Name", st.new_room_name.data(), st.new_room_name.size() + 1);
  ImGui::InputText("Description", st.new_room_desc.data(), st.new_room_desc.size() + 1);
  ImGui::InputInt("Duration (min)", &st.new_room_duration_min);
  ImGui::InputInt("Total questions", &st.new_room_q_total);
  ImGui::InputInt("Easy", &st.new_room_easy);
  ImGui::SameLine();
  ImGui::InputInt("Medium", &st.new_room_medium);
  ImGui::SameLine();
  ImGui::InputInt("Hard", &st.new_room_hard);
  if (ImGui::Button("Create")) {
    send_request(core, st, "CREATE_ROOM",
                 {{"room_name", st.new_room_name},
                  {"description", st.new_room_desc},
                  {"duration_minutes", st.new_room_duration_min},
                  {"question_settings",
                   {{"total_questions", st.new_room_q_total},
                    {"difficulty_distribution",
                     {{"easy", st.new_room_easy}, {"medium", st.new_room_medium}, {"hard", st.new_room_hard}}}}}});
  }

  ImGui::Separator();
  ImGui::Text("Rooms");
  if (ImGui::Button("Refresh")) {
    send_request(core, st, "LIST_ROOMS", {{"filter", {{"status", "WAITING"}}}});
  }

  if (ImGui::BeginTable("rooms", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("ID");
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Code");
    ImGui::TableSetupColumn("Status");
    ImGui::TableSetupColumn("Actions");
    ImGui::TableHeadersRow();
    for (auto& r : st.rooms) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0); ImGui::Text("%d", r.room_id);
      ImGui::TableSetColumnIndex(1); ImGui::Text("%s", r.room_name.c_str());
      ImGui::TableSetColumnIndex(2); ImGui::Text("%s", r.room_code.c_str());
      ImGui::TableSetColumnIndex(3); ImGui::Text("%s", r.status.c_str());
      ImGui::TableSetColumnIndex(4);
      ImGui::PushID(r.room_id);
      if (ImGui::Button("Join")) {
        send_request(core, st, "JOIN_ROOM", {{"room_id", r.room_id}});
      }
      ImGui::SameLine();
      if (ImGui::Button("Start")) {
        send_request(core, st, "START_EXAM", {{"room_id", r.room_id}});
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

void render_exam(ClientState& st, ClientCore& core) {
  ImGui::Begin("Exam");
  static int room_id = 0;
  ImGui::InputInt("Room id", &room_id);
  ImGui::SameLine();
  if (ImGui::Button("Get paper")) {
    send_request(core, st, "GET_EXAM_PAPER", {{"room_id", room_id}});
  }
  ImGui::Separator();
  ImGui::Text("Exam id: %d", st.exam.exam_id);
  int idx = 0;
  for (auto& q : st.exam.questions) {
    ImGui::PushID(idx++);
    ImGui::TextWrapped("%d) [%s][%s] %s", q.question_id, q.difficulty.c_str(), q.topic.c_str(), q.text.c_str());
    ImGui::Spacing();
    for (auto& opt : q.options) {
      if (ImGui::RadioButton((opt.first + "##" + std::to_string(q.question_id)).c_str(),
                             q.answer == opt.first)) {
        q.answer = opt.first;
      }
      ImGui::SameLine();
      ImGui::Text("%s", opt.second.c_str());
    }
    ImGui::Separator();
    ImGui::PopID();
  }
  if (ImGui::Button("Submit exam")) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& q : st.exam.questions) {
      arr.push_back({{"question_id", q.question_id}, {"selected_option", q.answer.empty() ? "A" : q.answer}});
    }
    send_request(core, st, "SUBMIT_EXAM", {{"exam_id", st.exam.exam_id}, {"final_answers", arr}});
  }
  ImGui::End();
}

void render_practice(ClientState& st, ClientCore& core) {
  ImGui::Begin("Practice");
  static int qcount = 6;
  static int dur = 10;
  ImGui::InputInt("Question count", &qcount);
  ImGui::InputInt("Duration (min)", &dur);
  if (ImGui::Button("Start practice")) {
    send_request(core, st, "START_PRACTICE",
                 {{"question_count", qcount},
                  {"duration_minutes", dur},
                  {"difficulty_filter", nlohmann::json::array({"EASY", "MEDIUM"})},
                  {"topic_filter", nlohmann::json::array({"Networking"})}});
  }
  ImGui::Text("Practice id: %d", st.practice.practice_id);
  int idx = 0;
  for (auto& q : st.practice.questions) {
    ImGui::PushID(idx++);
    ImGui::TextWrapped("%d) [%s][%s] %s", q.question_id, q.difficulty.c_str(), q.topic.c_str(), q.text.c_str());
    ImGui::Spacing();
    for (auto& opt : q.options) {
      if (ImGui::RadioButton((opt.first + "##p" + std::to_string(q.question_id)).c_str(),
                             q.answer == opt.first)) {
        q.answer = opt.first;
      }
      ImGui::SameLine();
      ImGui::Text("%s", opt.second.c_str());
    }
    ImGui::Separator();
    ImGui::PopID();
  }
  if (ImGui::Button("Submit practice")) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& q : st.practice.questions) {
      arr.push_back({{"question_id", q.question_id}, {"selected_option", q.answer.empty() ? "A" : q.answer}});
    }
    send_request(core, st, "SUBMIT_PRACTICE", {{"practice_id", st.practice.practice_id}, {"final_answers", arr}});
  }
  ImGui::End();
}

void render_results_history(ClientState& st, ClientCore& core) {
  ImGui::Begin("Results / History");
  static int room_id = 0;
  ImGui::InputInt("Room for results", &room_id);
  ImGui::SameLine();
  if (ImGui::Button("Get room results")) {
    send_request(core, st, "GET_ROOM_RESULTS", {{"room_id", room_id}});
  }
  ImGui::SameLine();
  if (ImGui::Button("Get my history")) {
    send_request(core, st, "GET_USER_HISTORY", nlohmann::json::object());
  }
  ImGui::Separator();
  ImGui::TextWrapped("Last results:\n%s", st.last_results.c_str());
  ImGui::Separator();
  ImGui::TextWrapped("Last errors:\n%s", st.last_errors.c_str());
  ImGui::End();
}

}  // namespace

// ---------------------------------------------------------------------------
bool render_ui(ClientState& state, ClientCore& core) {
  static bool initialized = false;
  static SDL_Window* window = nullptr;
  static SDL_Renderer* renderer = nullptr;
  if (!initialized) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
      std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
      return false;
    }
    window = SDL_CreateWindow("Quiz Client", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    initialized = true;
  }

  bool running = true;
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    ImGui_ImplSDL2_ProcessEvent(&event);
    if (event.type == SDL_QUIT) running = false;
  }

  while (auto ev = core.pop_event()) {
    handle_event(state, *ev);
  }

  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  render_header(state, core);
  render_rooms(state, core);
  render_exam(state, core);
  render_practice(state, core);
  render_results_history(state, core);

  ImGui::Render();
  SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
  SDL_RenderClear(renderer);
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
  SDL_RenderPresent(renderer);

  return running;
}

}  // namespace quiz::client
