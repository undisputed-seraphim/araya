#pragma once

#include "araya/plugin.hpp"

// The model-facing `lsp` tool over the `lsp` seam: one read-only tool with four
// operations, one-based cursor coordinates, result caps, and a per-call
// timeout. A feature-replication of the deepseek-harness
// `@deepseek-ai/dsh-tool-lsp`.
namespace araya::tool_lsp {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_lsp
