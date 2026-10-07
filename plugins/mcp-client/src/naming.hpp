#pragma once

#include <string>
#include <string_view>

namespace araya::mcp {

// The model-facing public name for one MCP tool: `mcp__<server>__<rawName>`
// normalized to the function-name contract (`[A-Za-z0-9_-]`, at most 64
// chars). When character replacement or truncation changes the name, a
// 12-hex-char SHA-256 of `(server, rawName)` is appended so distinct
// identities never collapse. Deterministic and stable across restarts.
std::string public_tool_name(std::string_view server, std::string_view raw_name);

} // namespace araya::mcp
