#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/message.hpp"
#include "server/thread_pool.hpp"

namespace quiz::server {

class Connection;

using HandlerFn = std::function<quiz::Message(const quiz::Message&)>;

class Server {
 public:
  Server(std::string host, uint16_t port, std::size_t workers = 4);
  ~Server();

  void register_handler(const std::string& action, HandlerFn handler);

  bool start();
  void stop();
  void run();  // blocking loop until stop is requested (Ctrl+C).

  void handle_message(const std::shared_ptr<Connection>& conn,
                      const quiz::Message& msg);

 private:
  void accept_loop();
  void close_all_connections();

  std::string host_;
  uint16_t port_;
  std::atomic<bool> running_{false};
  int listen_fd_{-1};
  std::thread accept_thread_;

  std::mutex conns_mtx_;
  std::vector<std::shared_ptr<Connection>> connections_;

  ThreadPool workers_;
  std::mutex handlers_mtx_;
  std::map<std::string, HandlerFn> handlers_;
};

class Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(int fd, Server* server, std::string peer);
  ~Connection();

  void start();
  void stop();
  void send(const quiz::Message& msg);
  std::string peer() const { return peer_; }

 private:
  void read_loop();

  int fd_;
  Server* server_;
  std::string peer_;
  std::atomic<bool> alive_{true};
  std::thread reader_;
  std::mutex send_mtx_;
};

}  // namespace quiz::server
