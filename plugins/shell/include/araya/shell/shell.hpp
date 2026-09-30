#pragma once

#include "araya/plugin.hpp"

// The one-shot shell tool: `bash` runs a command in a fresh shell and
// returns its combined output with exit/signal/timeout markers. A trimmed
// port of the harness's `dsh-tool-bash`: capped output is truncated (not
// spilled). When the optional `sandbox` and `sandbox-policy` services are
// mounted, each call runs confined under the resolved file-effect policy
// (Landlock, applied in the child before exec), and a denial is reported as
// the model-facing marker; without them the tool is unconstrained.
namespace araya::shell {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::shell
