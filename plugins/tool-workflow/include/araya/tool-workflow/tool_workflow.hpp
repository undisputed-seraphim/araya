#pragma once

#include "araya/plugin.hpp"

// The model-facing `workflow` tool: run a JavaScript orchestration script
// that fans out subagents and return the script's final value. The engine
// (script parsing, execution, caps, cancellation) lives behind the
// `workflow` service; this plugin owns only the model-facing schema, the
// usage-guidance section, and the result rendering.
namespace araya::tool_workflow {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_workflow
