#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace quiz::server {

class ThreadPool {
 public:
  explicit ThreadPool(std::size_t workers);
  ~ThreadPool();

  void enqueue(std::function<void()> task);
  void shutdown();

 private:
  void worker_loop();

  std::mutex mtx_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> tasks_;
  bool stopping_{false};
  std::vector<std::thread> threads_;
};

}  // namespace quiz::server
