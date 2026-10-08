#pragma once

#include "araya/plugin.hpp"

namespace araya::tool_session_query {

// The plugin descriptor: provides no service; dependencies are `tools`,
// `system-prompt`, `session-query`, `sessions`, and `agent` (the open-turn
// boundary). Registers the five session query tools and their section.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_session_query
