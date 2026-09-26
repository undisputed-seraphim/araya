#pragma once

#include "araya/plugin.hpp"

// The model-facing `web_fetch` and `web_search` tools over the web service.
// A feature-replication of the deepseek-harness `@deepseek-ai/dsh-tool-web`,
// trimmed to the in-process shape (plain-text fetch rendering, no structured
// output metadata or result cards).
namespace araya::tool_web {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_web
