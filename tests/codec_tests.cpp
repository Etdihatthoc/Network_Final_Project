#include <iostream>
#include <string>
#include <vector>
#include <cstring>

#include "common/codec.hpp"

using quiz::Message;
using quiz::MessageType;
using quiz::Status;

namespace {

struct TestRunner {
  int failures{0};

  void expect(bool condition, const std::string& msg) {
    if (!condition) {
      ++failures;
      std::cerr << "[FAIL] " << msg << "\n";
    }
  }

  int exit_code() const {
    if (failures == 0) {
      std::cout << "[PASS] all codec tests\n";
      return 0;
    }
    std::cerr << "[FAILURES] total: " << failures << "\n";
    return 1;
  }
};

}  // namespace

int main() {
  TestRunner tr;

  // Round-trip basic message.
  {
    Message msg;
    msg.type = MessageType::Request;
    msg.action = "LOGIN";
    msg.timestamp = 1700000000;
    msg.data = {{"username", "user"}, {"password", "hashed"}};

    std::string err;
    auto frame = quiz::encode_frame(msg, err);
    tr.expect(!frame.empty(), "encode basic message");

    Message decoded;
    bool ok = quiz::decode_frame(frame, decoded, err);
    tr.expect(ok, "decode basic message");
    tr.expect(decoded.action == msg.action, "action preserved");
    tr.expect(decoded.type == msg.type, "message_type preserved");
    tr.expect(decoded.data == msg.data, "data preserved");
  }

  // Large payload near limit.
  {
    Message msg;
    msg.type = MessageType::Request;
    msg.action = "PUT_LARGE";
    msg.timestamp = 1700000001;
    msg.data["blob"] = std::string(900'000, 'a');  // under 1MiB

    std::string err;
    auto frame = quiz::encode_frame(msg, err);
    tr.expect(!frame.empty(), "encode large payload");

    Message decoded;
    bool ok = quiz::decode_frame(frame, decoded, err);
    tr.expect(ok, "decode large payload");
    tr.expect(decoded.data["blob"].get<std::string>().size() ==
                  msg.data["blob"].get<std::string>().size(),
              "blob size preserved");
  }

  // Invalid UTF-8 payload.
  {
    std::string bad;
    bad.push_back(static_cast<char>(0xC3));  // Invalid lead without continuation.
    bad.push_back(static_cast<char>(0x28));
    std::uint32_t len = static_cast<std::uint32_t>(bad.size());
    std::uint32_t be = ((len & 0x000000FFu) << 24) | ((len & 0x0000FF00u) << 8) |
                       ((len & 0x00FF0000u) >> 8) | ((len & 0xFF000000u) >> 24);
    std::vector<std::uint8_t> frame(quiz::kFramePrefixBytes + bad.size());
    std::memcpy(frame.data(), &be, sizeof(be));
    std::memcpy(frame.data() + quiz::kFramePrefixBytes, bad.data(), bad.size());

    quiz::Message decoded;
    std::string err;
    bool ok = quiz::decode_frame(frame, decoded, err);
    tr.expect(!ok, "detect invalid UTF-8");
  }

  // Length mismatch detection.
  {
    Message msg;
    msg.type = MessageType::Request;
    msg.action = "PING";
    msg.timestamp = 1700000002;
    msg.data = {{"ping", true}};

    std::string err;
    auto frame = quiz::encode_frame(msg, err);
    tr.expect(!frame.empty(), "encode for length mismatch test");
    if (!frame.empty()) {
      frame[3] = static_cast<std::uint8_t>(frame[3] - 1);  // corrupt length
    }
    Message decoded;
    bool ok = quiz::decode_frame(frame, decoded, err);
    tr.expect(!ok, "detect length mismatch");
  }

  return tr.exit_code();
}
