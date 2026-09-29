#pragma once

#include "araya/plugin.hpp"

// The model-facing `skill` loader over the skill registry, plus the session
// skill catalog contributed as an agent context producer (a durable pre-step
// user message). A feature-replication of the deepseek-harness
// `@deepseek-ai/dsh-tool-skill`, trimmed to the in-process shape.
//
// Divergence from the harness (recorded): the catalog is a driver-composed
// context message rather than a raw `agent/pre-step` listener, and there is no
// user-explicit `/<skill>` invocation gesture.
namespace araya::tool_skill {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_skill
