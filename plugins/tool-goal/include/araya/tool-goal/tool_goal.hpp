#pragma once

#include "araya/plugin.hpp"

// The model-facing goal tools (`get_goal`, `create_goal`, `update_goal`) over
// the same-session goal domain, plus the `tool:goal` prompt section. A
// feature-replication of the deepseek-harness `@deepseek-ai/dsh-tool-goal`,
// trimmed to the three tools, their policy section, and the execution-time
// authority checks (no presentation cards, no deferred context wrap-up).
namespace araya::tool_goal {

// The plugin descriptor: requires `agents`/`sessions`/`goals`/`system-prompt`/
// `tools`; registers the goal tools and the `tool:goal` section.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_goal
