#pragma once

#include "client/state.hpp"
#include "client/core.hpp"

namespace quiz::client {

// Render UI (stubbed ImGui). Returns false if user requested exit.
bool render_ui(ClientState& state, ClientCore& core);

}  // namespace quiz::client
