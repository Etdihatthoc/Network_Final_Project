#include "server/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

#include "common/codec.hpp"

namespace quiz::server {

namespace {

std::string now_ts() {
  using namespace std::chrono;
  auto sec = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  return std::to_string(sec);
}

int create_listen_socket(const std::string& host, uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return -1;
  }
  int opt = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
    std::cerr << "[server] invalid host: " << host << "\n";
    ::close(fd);
    return -1;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("bind");
    ::close(fd);
    return -1;
  }
  if (::listen(fd, 64) < 0) {
    std::perror("listen");
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string peer_addr(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    char buf[64];
    ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    std::ostringstream oss;
    oss << buf << ":" << ntohs(addr.sin_port);
    return oss.str();
  }
  return "unknown";
}

Message make_error(const Message& req,
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

}  // namespace

Server::Server(std::string host, uint16_t port, std::size_t workers)
    : host_(std::move(host)), port_(port), workers_(workers) {}

Server::~Server() {
  stop();
}

void Server::register_handler(const std::string& action, HandlerFn handler) {
  std::lock_guard<std::mutex> lock(handlers_mtx_);
  handlers_[action] = std::move(handler);
}

bool Server::start() {
  if (running_.load()) return true;
  listen_fd_ = create_listen_socket(host_, port_);
  if (listen_fd_ < 0) return false;
  running_.store(true);
  accept_thread_ = std::thread(&Server::accept_loop, this);
  return true;
}

void Server::stop() {
  if (!running_.exchange(false)) return;
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  close_all_connections();
  workers_.shutdown();
}

void Server::run() {
  if (!start()) {
    std::cerr << "[server] failed to start\n";
  }
}

void Server::accept_loop() {
  while (running_.load()) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      if (!running_.load()) break;
      std::perror("accept");
      continue;
    }
    auto conn = std::make_shared<Connection>(client_fd, this, peer_addr(client_fd));
    {
      std::lock_guard<std::mutex> lock(conns_mtx_);
      connections_.push_back(conn);
    }
    conn->start();
    std::cout << "[server] new connection from " << conn->peer() << "\n";
  }
}

void Server::handle_message(const std::shared_ptr<Connection>& conn,
                            const Message& msg) {
  std::cout << "[DEBUG] handle_message enqueuing task for action=" << msg.action << "\n";
  workers_.enqueue([this, conn, msg] {
    std::cout << "[DEBUG] worker processing action=" << msg.action << "\n";
    HandlerFn handler;
    {
      std::lock_guard<std::mutex> lock(handlers_mtx_);
      auto it = handlers_.find(msg.action);
      if (it != handlers_.end()) {
        handler = it->second;
      }
    }

    Message resp;
    if (!handler) {
      std::cout << "[DEBUG] no handler found for " << msg.action << "\n";
      resp = make_error(msg, "UNKNOWN_ACTION", "Action not supported");
    } else {
      try {
        std::cout << "[DEBUG] calling handler for " << msg.action << "\n";
        resp = handler(msg);
        std::cout << "[DEBUG] handler returned, status=" << (int)resp.status << "\n";
      } catch (const std::exception& ex) {
        std::cout << "[DEBUG] handler threw exception: " << ex.what() << "\n";
        resp = make_error(msg, "HANDLER_ERROR", ex.what());
      }
    }
    if (resp.type != MessageType::Response) {
      resp.type = MessageType::Response;
    }
    if (resp.action.empty()) resp.action = msg.action;
    if (resp.session_id.empty()) resp.session_id = msg.session_id;
    resp.timestamp = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::cout << "[DEBUG] sending response for " << msg.action << "\n";
    conn->send(resp);
    std::cout << "[DEBUG] response sent for " << msg.action << "\n";
  });
  std::cout << "[DEBUG] handle_message task enqueued\n";
}

void Server::close_all_connections() {
  std::vector<std::shared_ptr<Connection>> to_close;
  {
    std::lock_guard<std::mutex> lock(conns_mtx_);
    to_close.swap(connections_);
  }
  for (auto& c : to_close) {
    if (c) c->stop();
  }
}

Connection::Connection(int fd, Server* server, std::string peer)
    : fd_(fd), server_(server), peer_(std::move(peer)) {}

Connection::~Connection() {
  stop();
}

void Connection::start() {
  std::cout << "[DEBUG] Connection::start() for " << peer_ << "\n";
  reader_ = std::thread(&Connection::read_loop, this);
  std::cout << "[DEBUG] reader thread created\n";
}

void Connection::stop() {
  if (!alive_.exchange(false)) return;
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
  }
  if (reader_.joinable()) reader_.join();
}

void Connection::send(const Message& msg) {
  std::string error;
  auto frame = encode_frame(msg, error);
  if (frame.empty()) {
    std::cerr << "[server] encode error to " << peer_ << ": " << error << "\n";
    return;
  }
  std::lock_guard<std::mutex> lock(send_mtx_);
  if (!write_frame(fd_, frame, error)) {
    std::cerr << "[server] send error to " << peer_ << ": " << error << "\n";
  }
}

void Connection::read_loop() {
  std::cout << "[DEBUG] read_loop started for " << peer_ << "\n";
  while (alive_.load()) {
    std::vector<std::uint8_t> frame;
    std::string error;
    std::cout << "[DEBUG] waiting for frame from " << peer_ << "\n";
    if (!read_frame(fd_, frame, error)) {
      if (alive_.load()) {
        std::cerr << "[server] read error from " << peer_ << ": " << error << "\n";
      }
      break;
    }
    std::cout << "[DEBUG] got frame, decoding...\n";
    Message msg;
    if (!decode_frame(frame, msg, error)) {
      std::cerr << "[server] decode error from " << peer_ << ": " << error << "\n";
      continue;
    }
    std::cout << "[DEBUG] decoded msg action=" << msg.action << ", calling handler...\n";
    server_->handle_message(shared_from_this(), msg);
    std::cout << "[DEBUG] handler returned\n";
  }
  std::cout << "[DEBUG] read_loop exiting for " << peer_ << "\n";
  alive_.store(false);
}

}  // namespace quiz::server
