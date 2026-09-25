#pragma once

#include "araya/plugin.hpp"

// The one-shot shell tool: `bash` runs a command in a fresh shell and
// returns its combined output with exit/signal/timeout markers. A trimmed
// port of the harness's `dsh-tool-bash`: no sandbox, no background jobs,
// and no spill store (capped output is truncated, not saved).
namespace araya::shell {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::shell
