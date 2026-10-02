#pragma once

#include "araya/effects.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/task.hpp"

#include <boost/json/array.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
	// The id of the session whose agent called this tool (the harness's
	// `exec.agent`). Lets a tool that spawns work - subagents today -
	// establish lineage and inherit its caller's context. Empty when the
	// invocation has no owning session (direct API use, tests).
	std::string session;
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

// The common single-text-block result and its error counterpart. Every tool
// plugin would otherwise inline the same three lines.
inline tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

inline tool_result error_result(std::string text) { return text_result(std::move(text), true); }

inline tool_result error_result(std::exception const& e) { return error_result(std::string("Error: ") + e.what()); }

using tool_handler = std::function<araya::task<tool_result>(tool_context const&)>;

// A per-scope filter over the global tools a scope inherits. `allow` keeps
// only the named globals; `deny` removes the named globals; when both are
// set, a name must be allowed and not denied. Scoped registrations are never
// filtered. Restrictions intersect and, like every registration, are owned by
// the registering caller.
struct tool_restriction {
	std::optional<std::vector<std::string>> allow;
	std::optional<std::vector<std::string>> deny;
};

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

	// Restricts the global tools visible in `scope` (scoped registrations are
	// unaffected). Requires a non-empty scope; an empty filter, or one naming
	// an unknown global tool, throws. Restrictions intersect and are owned by
	// `caller`.
	araya::registration restrict(
		araya::plugin_context& caller,
		tool_restriction filter,
		std::optional<std::string> scope = {});

private:
	struct tool_entry {
		std::uint64_t id = 0;
		std::optional<std::string> scope;
		tool_definition definition;
		tool_handler handler;
	};
	struct restriction_entry {
		std::uint64_t id = 0;
		std::optional<std::string> scope;
		tool_restriction filter;
	};

	// Merged visible entries (scoped shadows global), name-ordered.
	std::vector<tool_entry const*> visible(std::optional<std::string> const& scope) const;

	// Whether every restriction in `scope` admits the global `name`.
	bool admits(std::string_view name, std::optional<std::string> const& scope) const;

	std::uint64_t next_id_ = 1;
	std::vector<tool_entry> tools_;
	std::uint64_t next_restriction_id_ = 1;
	std::vector<restriction_entry> restrictions_;
};

inline constexpr araya::service_key<tools_service> tools_key{"tools", 1};

// The plugin descriptor: requires `system-prompt`, provides `tools`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tools
