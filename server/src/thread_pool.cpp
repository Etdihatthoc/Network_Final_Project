#include "server/thread_pool.hpp"

namespace quiz::server {

ThreadPool::ThreadPool(std::size_t workers) {
  if (workers == 0) workers = 1;
  threads_.reserve(workers);
  for (std::size_t i = 0; i < workers; ++i) {
    threads_.emplace_back(&ThreadPool::worker_loop, this);
  }
}

ThreadPool::~ThreadPool() {
  shutdown();
}

void ThreadPool::enqueue(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (stopping_) return;
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
}

void ThreadPool::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (stopping_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
}

void ThreadPool::worker_loop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mtx_);
      cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

}  // namespace quiz::server
