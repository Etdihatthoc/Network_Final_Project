#pragma once

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

namespace spdlog {

namespace level {
enum level_enum { trace, debug, info, warn, err, critical, off };
}

class logger {
 public:
  explicit logger(std::string name) : name_(std::move(name)) {}

  template <typename... Args>
  void info(const std::string& msg, Args&&... /*args*/) { log("INFO", msg); }

  template <typename... Args>
  void warn(const std::string& msg, Args&&... /*args*/) { log("WARN", msg); }

  template <typename... Args>
  void error(const std::string& msg, Args&&... /*args*/) { log("ERROR", msg); }

 private:
  void log(const char* level, const std::string& msg) {
    std::cout << "[" << level << "] " << msg << std::endl;
  }

  std::string name_;
};

inline std::shared_ptr<logger> rotating_logger_mt(const std::string& name,
                                                  const std::string& /*filename*/,
                                                  std::size_t /*max_size*/,
                                                  std::size_t /*max_files*/) {
  return std::make_shared<logger>(name);
}

inline void set_default_logger(std::shared_ptr<logger> /*lg*/) {}
inline void set_level(level::level_enum /*lvl*/) {}
inline void set_pattern(const std::string& /*pattern*/) {}

}  // namespace spdlog
