#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"
#include "araya/task.hpp"

#include <boost/json/value.hpp>

#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// The MCP client service: connect one or more Model Context Protocol servers
// (stdio today; streamable-http later), discover their tools, and register
// them into the tool registry under `mcp__<server>__<rawName>`. A
// feature-replication of the deepseek-harness `@deepseek-ai/dsh-mcp-client`,
// trimmed to a hand-rolled JSON-RPC 2.0 subset (protocol 2025-06-18) over a
// stdio child process.
//
// Configuration is a JSON document (the shared config_file-or-inline
// convention) with a `servers` object keyed by server name, plus shared
// timeout / reconnect / instruction-cap defaults. See doc/CONFIG.md.
//
// Divergences from DSH (recorded in TODO.md): no MCP SDK; no sampling,
// elicitation, roots, prompts, logging, task-based execution, or resource
// subscriptions; registration is global (server-qualified) rather than
// per-Agent; `structuredContent` folds to text; `${VAR}` interpolation rather
// than `!!js`.
namespace araya::mcp {

// One resource operation against a named server, routed by `mcp-resources`
// (M13). `method` is `resources/list`, `resources/templates/list`, or
// `resources/read`; `uri`/`cursor` are the method's arguments.
struct resource_request {
	std::string method;
	std::string uri;
	std::string cursor;
};

// The MCP client service. Owns the live connections; `mcp-resources` consumes
// it to reach server resources.
class mcp_service {
public:
	virtual ~mcp_service() = default;

	// The configured, active server names (sorted).
	virtual std::vector<std::string> servers() const = 0;

	// Runs one MCP method against `server` and returns the protocol result as
	// JSON. Throws std::runtime_error when the server is unknown or the call
	// fails (JSON-RPC error, timeout, or a lost connection).
	virtual araya::task<boost::json::value>
	request(std::string const& server, std::string_view method, boost::json::value params, std::stop_token stop) = 0;
};

inline constexpr araya::service_key<mcp_service> mcp_key{"mcp", 1};

// The plugin descriptor: requires `tools` and `system-prompt`, provides `mcp`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::mcp
