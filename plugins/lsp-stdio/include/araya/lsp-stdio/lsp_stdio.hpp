#pragma once

#include "araya/plugin.hpp"

// The stdio language-server provider for the `lsp` seam: configured server
// commands and extension mappings, pooled one process per workspace, transient
// didOpen/query/didClose lifecycles. A feature-replication of the
// deepseek-harness `@deepseek-ai/dsh-lsp-stdio` (Linux/local only).
namespace araya::lsp_stdio {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::lsp_stdio
