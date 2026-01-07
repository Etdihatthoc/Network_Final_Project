#include <iostream>
#include <string>

#include "client/core.hpp"
#include "client/state.hpp"
#include "client/ui.hpp"
#include "common/message.hpp"

int main() {
  using namespace quiz;
  quiz::client::ClientCore core;
  quiz::client::ClientState state;

  std::cout << "[client] stub UI running in console. Use menu to interact.\n";
  while (quiz::client::render_ui(state, core)) {
    // loop until user quits
  }
  return 0;
}
