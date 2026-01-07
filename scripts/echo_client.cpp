#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "common/codec.hpp"

using quiz::Message;
using quiz::MessageType;
using quiz::Status;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <host> <port> [message]\n";
    return 1;
  }
  std::string host = argv[1];
  uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
  std::string msg_text = (argc > 3) ? argv[3] : "hello";

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return 1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
    std::cerr << "Invalid host\n";
    return 1;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("connect");
    return 1;
  }

  Message req;
  req.type = MessageType::Request;
  req.action = "ECHO";
  req.timestamp = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  req.data = {{"msg", msg_text}};

  std::string err;
  auto frame = quiz::encode_frame(req, err);
  if (frame.empty()) {
    std::cerr << "encode error: " << err << "\n";
    return 1;
  }
  if (!quiz::write_frame(fd, frame, err)) {
    std::cerr << "write error: " << err << "\n";
    return 1;
  }

  std::vector<std::uint8_t> resp_frame;
  if (!quiz::read_frame(fd, resp_frame, err)) {
    std::cerr << "read error: " << err << "\n";
    return 1;
  }
  Message resp;
  if (!quiz::decode_frame(resp_frame, resp, err)) {
    std::cerr << "decode error: " << err << "\n";
    return 1;
  }
  std::cout << "Response status: " << (resp.status == Status::Success ? "SUCCESS" : "ERROR")
            << " data=" << resp.data.dump() << "\n";

  ::close(fd);
  return 0;
}
