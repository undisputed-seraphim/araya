#pragma once

#include "araya/plugin.hpp"

// The model-facing `skill` loader over the skill registry, plus the session
// skill catalog rendered as a system-prompt section. A feature-replication of
// the deepseek-harness `@deepseek-ai/dsh-tool-skill`, trimmed to the in-process
// shape.
//
// Divergence from the harness (recorded): the catalog is a section rather than
// an `agent/pre-step`-injected user message, and there is no user-explicit
// `/<skill>` invocation gesture.
namespace araya::tool_skill {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_skill
