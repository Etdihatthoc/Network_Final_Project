#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/message.hpp"

namespace quiz {

constexpr std::size_t kFramePrefixBytes = 4;
constexpr std::size_t kMaxPayloadSize = 1024 * 1024;  // 1 MiB safeguard.

// Low-level helpers for POSIX-style file descriptors.
// Returns total bytes read (0 means EOF) or -1 on unrecoverable error.
ssize_t read_exact(int fd, void* buffer, std::size_t length);
// Returns total bytes written or -1 on unrecoverable error.
ssize_t write_exact(int fd, const void* buffer, std::size_t length);

// Encode a Message into a length-prefixed frame.
// On success, returns frame (prefix + JSON). On failure, frame is empty and error is filled.
std::vector<std::uint8_t> encode_frame(const Message& msg, std::string& error);

// Decode a full frame (prefix + payload). Returns true on success, false otherwise.
bool decode_frame(const std::vector<std::uint8_t>& frame, Message& out, std::string& error);

// Read a frame from fd into `frame` (prefix + payload). Returns true on success, false on EOF/error.
bool read_frame(int fd, std::vector<std::uint8_t>& frame, std::string& error);

// Write a fully encoded frame to fd. Returns true on success.
bool write_frame(int fd, const std::vector<std::uint8_t>& frame, std::string& error);

}  // namespace quiz
