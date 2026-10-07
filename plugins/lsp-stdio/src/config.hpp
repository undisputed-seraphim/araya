#pragma once

#include "araya/plugin.hpp"

#include <boost/json/value.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The lsp-stdio configuration model, parsed from the JSON document carried by
// the `config_file`/`config` plugin keys:
//
//   { "servers": { "typescript": {
//       "command": "typescript-language-server",
//       "args": ["--stdio"],
//       "extensionToLanguage": { ".ts": "typescript" },
//       "env": { "NODE_OPTIONS": "--max-old-space-size=4096" },
//       "initializationOptions": null,
//       "configuration": null,
//       "maxMessageBytes": 16000000,
//       "maxStderrBytes": 1000000,
//       "maxDocumentBytes": 4000000,
//       "shutdownTimeoutMs": 5000,
//       "killGraceMs": 2000,
//       "requestTimeoutMs": 30000
//     } } }
namespace araya::lsp_stdio {

struct server_config {
	std::string id;
	std::string command;
	std::vector<std::string> args;
	// Provider-set per instance: the canonical workspace directory (not parsed
	// from config).
	std::string cwd;
	// Lowercase leading-dot extension -> language id, insertion order.
	std::vector<std::pair<std::string, std::string>> extension_to_language;
	std::vector<std::pair<std::string, std::string>> env;
	boost::json::value initialization_options; // null by default
	boost::json::value configuration;		   // null by default
	std::size_t max_message_bytes = 16'000'000;
	std::size_t max_stderr_bytes = 1'000'000;
	std::size_t max_document_bytes = 4'000'000;
	std::uint64_t shutdown_timeout_ms = 5'000;
	std::uint64_t kill_grace_ms = 2'000;
	// Araya addition: per-request timeout so a wedged server cannot hang a
	// query forever (DSH has no such knob; its tool owns the budget).
	std::uint64_t request_timeout_ms = 30'000;
};

struct lsp_stdio_config {
	std::vector<server_config> servers; // insertion order
	// Whether built-in default servers (clangd, rust-analyzer, ...) are merged
	// under the configured ones for extensions the user did not claim.
	bool defaults = true;
};

// Parses the plugin's JSON config. Throws std::invalid_argument on malformed
// JSON, a bad shape, or an invalid server entry.
lsp_stdio_config parse_config(araya::plugin_config const& config);

// Expands `${NAME}` references from the process environment.
std::string expand_vars(std::string_view text);

// Whether an ambient env name is credential-shaped and must be scrubbed.
bool sensitive_env_name(std::string_view name);

} // namespace araya::lsp_stdio
