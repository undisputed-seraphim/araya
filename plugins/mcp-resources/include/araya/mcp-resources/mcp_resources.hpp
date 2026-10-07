#pragma once

#include "araya/plugin.hpp"

// The shared MCP resource tools: `list_mcp_resources`,
// `list_mcp_resource_templates`, and `read_mcp_resource`, plus the
// `## MCP resource servers` prompt section. A feature-replication of the
// deepseek-harness `@deepseek-ai/dsh-mcp-resources`, trimmed to the single
// in-process `mcp` service (M12). Each tool names its server explicitly; the
// tools and section exist only while at least one server is configured.
namespace araya::mcp_resources {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::mcp_resources
