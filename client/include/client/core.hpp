#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

#include "common/message.hpp"

namespace quiz::client {

struct ClientEvent {
  Message message;
};

class ClientCore {
 public:
  ClientCore();
  ~ClientCore();

  bool connect(const std::string& host, uint16_t port);
  void disconnect();

  bool send_message(const Message& msg, std::string& error);

  std::optional<ClientEvent> pop_event();

  bool is_connected() const { return connected_; }

 private:
  void reader_loop();

  int fd_{-1};
  std::atomic<bool> connected_{false};
  std::thread reader_;

  std::mutex queue_mtx_;
  std::condition_variable queue_cv_;
  std::queue<ClientEvent> queue_;
};

}  // namespace quiz::client
