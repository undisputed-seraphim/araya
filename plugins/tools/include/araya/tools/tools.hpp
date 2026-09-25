#pragma once

#include "araya/effects.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/task.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The tool registry: schemas, per-scope visibility, and execution. Tool
// schemas feed the system-prompt assembly through the registry's tool
// providers (canonical order applied there); the agent loop executes by
// name through this service.
//
// Threading: strand-confined like every service.
namespace araya::tools {

// One registered tool's model-facing definition.
struct tool_definition {
	std::string name;
	std::string description;
	boost::json::value parameters; // JSON Schema object
};

struct tool_context {
	std::string call_id;
	std::string name;
	// The model's arguments: parsed JSON when they were valid, the raw
	// text as a JSON string otherwise (the harness keeps invalid JSON as
	// text rather than dropping the call).
	boost::json::value arguments;
	std::stop_token stop;
};

struct tool_result {
	// The tool's answer as session content blocks (e.g.
	// [{"type":"text","text":"..."}]).
	boost::json::value content;
	bool is_error = false;
};

using tool_handler = std::function<araya::task<tool_result>(tool_context const&)>;

class tools_service {
public:
	// Registers a tool; a duplicate name in the same scope throws. A
	// non-empty `scope` shadows a same-named global for that scope only.
	// The registration is owned by `caller`.
	araya::registration register_tool(
		araya::plugin_context& caller,
		tool_definition definition,
		tool_handler handler,
		std::optional<std::string> scope = {});

	// The visible definition for a name (global + the matching scope), or
	// nullopt.
	std::optional<tool_definition> find(std::string_view name, std::optional<std::string> const& scope = {}) const;

	// Every visible tool, sorted by name.
	std::vector<tool_definition> list(std::optional<std::string> const& scope = {}) const;

	// Executes the visible handler for `name`; nullopt when no tool is
	// visible under that name.
	araya::task<std::optional<tool_result>>
	invoke(std::string_view name, tool_context context, std::optional<std::string> const& scope = {}) const;

	// The schemas the system-prompt registry consumes for one scope.
	std::vector<system_prompt::tool_schema> schemas(std::optional<std::string> const& scope) const;

private:
	struct tool_entry {
		std::uint64_t id = 0;
		std::optional<std::string> scope;
		tool_definition definition;
		tool_handler handler;
	};

	// Merged visible entries (scoped shadows global), name-ordered.
	std::vector<tool_entry const*> visible(std::optional<std::string> const& scope) const;

	std::uint64_t next_id_ = 1;
	std::vector<tool_entry> tools_;
};

inline constexpr araya::service_key<tools_service> tools_key{"tools", 1};

// The plugin descriptor: requires `system-prompt`, provides `tools`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tools
