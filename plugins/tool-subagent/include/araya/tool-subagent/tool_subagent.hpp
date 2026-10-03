#pragma once

#include "araya/plugin.hpp"

// The model-facing delegation tool, one plugin instance per seam provider
// (each with a distinct `tool_name`), plus optional `list_subagent_models`
// discovery. The shared control tools live in a separate singleton
// araya::tool-subagent-control plugin.
namespace araya::tool_subagent {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_subagent
