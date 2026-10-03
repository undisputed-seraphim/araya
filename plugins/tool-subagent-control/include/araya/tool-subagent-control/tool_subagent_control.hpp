#pragma once

#include "araya/plugin.hpp"

// The global continuable-child control tools: `send_message`,
// `interrupt_agent`, and `list_agents`. Mounted once, apart from the
// provider-bound `tool-subagent` instances, so every delegation tool shares a
// single control API and the fixed tool names never collide.
namespace araya::tool_subagent_control {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_subagent_control
