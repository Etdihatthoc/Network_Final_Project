#include "client/core.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

#include "common/codec.hpp"

namespace quiz::client {

ClientCore::ClientCore() = default;

ClientCore::~ClientCore() {
  disconnect();
}

bool ClientCore::connect(const std::string& host, uint16_t port) {
  disconnect();
  fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ < 0) {
    std::perror("socket");
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
    std::cerr << "Invalid host\n";
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("connect");
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  connected_.store(true);
  reader_ = std::thread(&ClientCore::reader_loop, this);
  return true;
}

void ClientCore::disconnect() {
  if (!connected_.exchange(false)) return;
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
  }
  if (reader_.joinable()) reader_.join();
}

bool ClientCore::send_message(const Message& msg, std::string& error) {
  if (!connected_.load()) {
    error = "not connected";
    return false;
  }
  auto frame = encode_frame(msg, error);
  if (frame.empty()) return false;
  if (!write_frame(fd_, frame, error)) return false;
  return true;
}

std::optional<ClientEvent> ClientCore::pop_event() {
  std::unique_lock<std::mutex> lock(queue_mtx_);
  if (queue_.empty()) return std::nullopt;
  auto ev = queue_.front();
  queue_.pop();
  return ev;
}

void ClientCore::reader_loop() {
  while (connected_.load()) {
    std::vector<std::uint8_t> frame;
    std::string error;
    if (!read_frame(fd_, frame, error)) {
      if (connected_.load()) {
        std::cerr << "[client] read error: " << error << "\n";
      }
      break;
    }
    Message msg;
    if (!decode_frame(frame, msg, error)) {
      std::cerr << "[client] decode error: " << error << "\n";
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(queue_mtx_);
      queue_.push(ClientEvent{msg});
    }
    queue_cv_.notify_one();
  }
  connected_.store(false);
}

}  // namespace quiz::client
