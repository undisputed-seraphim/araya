#pragma once

#include "araya/plugin.hpp"

// The model-facing subagent tools: `subagent` (one instance per seam
// provider), plus the global continuable-child control tools
// `send_message`, `interrupt_agent`, and `list_agents`.
namespace araya::tool_subagent {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_subagent
