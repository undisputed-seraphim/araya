#pragma once

#include "araya/plugin.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The MCP client configuration model, parsed from the JSON document carried by
// the `config_file`/`config` plugin keys. Shape (OpenCode ergonomics + DSH
// semantics):
//
//   {
//     "servers": {
//       "github": {
//         "type": "local",                       // local (stdio) | remote
//         "command": ["github-mcp-server", "stdio"],
//         "environment": { "TOKEN": "${TOKEN}" },
//         "cwd": "/some/dir",
//         "enabled": true,
//         "timeoutMs": 60000,                    // per-call override
//         "maxInstructionBytes": 32768,
//         "failOnStartupError": false,
//         "reconnect": { "enabled": true, "initialDelayMs": 500,
//                        "maxDelayMs": 30000, "maxAttempts": 10 }
//       }
//     },
//     "requestTimeoutMs": 60000,
//     "maxInstructionBytes": 32768,
//     "reconnect": { ... }
//   }
//
// `${NAME}` references in strings expand from the process environment. `name`
// must match [A-Za-z0-9_-]{1,32} (it is the tool namespace).
namespace araya::mcp {

struct reconnect_config {
	bool enabled = true;
	std::uint64_t initial_delay_ms = 500;
	std::uint64_t max_delay_ms = 30'000;
	std::uint64_t max_attempts = 10;
};

struct server_config {
	std::string name;
	bool enabled = true;
	std::string transport = "local";							  // "local" (stdio) | "remote"
	std::vector<std::string> command;							  // local: program + args
	std::vector<std::pair<std::string, std::string>> environment; // local
	std::string cwd;											  // local
	std::string url;											  // remote
	std::vector<std::pair<std::string, std::string>> headers;	  // remote
	std::uint64_t timeout_ms = 60'000;
	std::uint64_t max_instruction_bytes = 32'768;
	bool fail_on_startup_error = false;
	reconnect_config reconnect;
};

struct mcp_config {
	std::vector<server_config> servers; // insertion order
	std::uint64_t request_timeout_ms = 60'000;
	std::uint64_t max_instruction_bytes = 32'768;
	bool fail_on_startup_error = false;
	reconnect_config reconnect;
};

// Parses the plugin's JSON config. Throws std::invalid_argument on malformed
// JSON, a bad shape, or an invalid server name.
mcp_config parse_config(araya::plugin_config const& config);

// Expands `${NAME}` references from the process environment (a missing name
// expands to the empty string).
std::string expand_vars(std::string_view text);

// Whether an ambient environment name is credential-shaped and must be
// scrubbed from a spawned server's environment.
bool sensitive_env_name(std::string_view name);

} // namespace araya::mcp
