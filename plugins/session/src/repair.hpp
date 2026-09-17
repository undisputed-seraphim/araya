#pragma once

#include "araya/session/session_types.hpp"

#include <vector>

namespace araya::session {

// Closes interrupted turns in a reconstructed log: every tool call an
// assistant block requested without a matching tool/result gets a
// synthesized closer (outcome "unknown"). Runs on the construction seed, so
// the closers are part of construction input and never publish.
void repair_interrupted_turns(std::vector<session_event>& log);

}  // namespace araya::session
