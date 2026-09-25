#pragma once

#include "araya/effects.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The system-prompt registry: ordered, scoped prompt sections, dynamic
// runtime contexts, tool-schema providers, and strict {{variable}}
// interpolation - a feature-replication of the deepseek-harness
// `@deepseek-ai/dsh-system-prompt` service, trimmed to the shape the
// araya loop consumes.
//
// Semantics carried over from the harness:
//   - sections concatenate in ascending `order`, ties broken by name;
//   - a scoped contribution shadows a same-named global one;
//   - `{{name}}` references interpolate strictly (unknown or undefined
//     names throw; a lone `{{` without a later `}}` is literal prose);
//   - empty sections disappear; non-empty ones join with a blank line;
//   - at most one effective `complete` section may be active, and it
//     becomes the whole prompt (after interceptors run);
//   - dynamic contexts render separately into one user-role snapshot.
//
// Threading: strand-confined like every service. assemble() is called by
// the agent loop on the control strand.
namespace araya::system_prompt {

// -- assembler context -----------------------------------------------------

// Everything one assembly may depend on. `scope` is an opaque agent scope
// id (a session id today); an empty scope sees global contributions only.
// provider/model/cwd back the built-in prompt variables.
struct assemble_context {
	std::optional<std::string> scope;
	std::string provider;
	std::string model;
	std::string cwd;
};

// -- assembly --------------------------------------------------------------

struct assembled_section {
	std::string name;
	std::string text;
	bool interpolate = true;
};

struct assembled_context {
	std::string name;
	std::string text;
};

// Tool schemas this provider contributes to one assembly.
struct tool_schema {
	std::string name;
	std::string description;
	boost::json::value parameters;
};

struct prompt_assembly {
	std::vector<assembled_section> sections;
	std::vector<assembled_context> contexts;
	std::vector<tool_schema> tools;
	std::map<std::string, std::optional<std::string>> variables;
};

// -- registry inputs -------------------------------------------------------

using render_fn = std::function<std::string(assemble_context const&)>;

// One contributed section. `render` takes precedence over the static
// `text` when set. `interpolate:false` preserves literal braces.
struct prompt_section {
	std::string name;
	int order = 0;
	std::string text;
	render_fn render;
	bool interpolate = true;
	bool complete = false;
};

// One dynamic runtime context (rendered into the superseding snapshot).
struct prompt_context {
	std::string name;
	int order = 0;
	std::string text;
	render_fn render;
};

using tool_provider = std::function<std::vector<tool_schema>(assemble_context const&)>;
using variable_provider = std::function<std::optional<std::string>(assemble_context const&)>;

// An ordered assembly interceptor: the araya stand-in for the harness's
// `system-prompt/assemble` waterfall. Runs after merge/sort and before the
// complete-section restore; mutates the assembly in place.
using assemble_interceptor = std::function<void(prompt_assembly&, assemble_context const&)>;

// -- the service -----------------------------------------------------------

class system_prompt_service {
public:
	// The built-in persona (config-backed, mutable at runtime). The
	// identity opener and runtime-context inclusion are switchable.
	void set_persona_prefix(std::string text) { persona_prefix_ = std::move(text); }
	void set_persona_suffix(std::string text) { persona_suffix_ = std::move(text); }
	std::string const& persona_prefix() const noexcept { return persona_prefix_; }
	std::string const& persona_suffix() const noexcept { return persona_suffix_; }
	void set_include_harness_identity(bool on) { include_harness_identity_ = on; }
	void set_include_runtime_context(bool on) { include_runtime_context_ = on; }
	void set_tool_order(std::vector<std::string> order) { tool_order_ = std::move(order); }

	// Registrations; each is owned by `caller` (unloading it removes the
	// contribution). A non-empty `scope` makes it scoped, shadowing a
	// same-named global contribution for that scope only.
	araya::registration
	section(araya::plugin_context& caller, prompt_section value, std::optional<std::string> scope = {});
	araya::registration
	context(araya::plugin_context& caller, prompt_context value, std::optional<std::string> scope = {});
	araya::registration variable(
		araya::plugin_context& caller,
		std::string name,
		variable_provider provider,
		std::optional<std::string> scope = {});
	araya::registration
	tools(araya::plugin_context& caller, tool_provider provider, std::optional<std::string> scope = {});
	araya::registration suppress_runtime_context(araya::plugin_context& caller, std::optional<std::string> scope = {});
	araya::registration
	intercept(araya::plugin_context& caller, int order, assemble_interceptor fn, std::optional<std::string> scope = {});

	// Merges global and matching scoped contributions, resolves variables,
	// collects and orders tool schemas, then runs the interceptors and
	// enforces any complete section.
	prompt_assembly assemble(assemble_context const& context = {}) const;

private:
	struct owned_section {
		std::uint64_t id = 0;
		std::optional<std::string> scope;
		prompt_section value;
	};
	struct owned_context {
		std::uint64_t id = 0;
		std::optional<std::string> scope;
		prompt_context value;
	};
	struct owned_variable {
		std::uint64_t id = 0;
		std::optional<std::string> scope;
		std::string name;
		variable_provider provider;
	};
	struct owned_tools {
		std::uint64_t id = 0;
		std::optional<std::string> scope;
		tool_provider provider;
	};
	struct owned_suppressor {
		std::uint64_t id = 0;
		std::optional<std::string> scope;
	};
	struct owned_interceptor {
		std::uint64_t id = 0;
		std::optional<std::string> scope;
		int order = 0;
		assemble_interceptor fn;
	};

	std::uint64_t next_id_ = 1;
	std::string persona_prefix_;
	std::string persona_suffix_;
	bool include_harness_identity_ = true;
	bool include_runtime_context_ = true;
	std::optional<std::vector<std::string>> tool_order_;
	std::vector<owned_section> sections_;
	std::vector<owned_context> contexts_;
	std::vector<owned_variable> variables_;
	std::vector<owned_tools> tool_providers_;
	std::vector<owned_suppressor> suppressors_;
	std::vector<owned_interceptor> interceptors_;
};

inline constexpr araya::service_key<system_prompt_service> system_prompt_key{"system-prompt", 1};

// The named built-in sections (exported so a composition can shadow them).
inline constexpr std::string_view harness_identity_section = "harness:identity";
inline constexpr std::string_view persona_prefix_section = "deployment:persona-prefix";
inline constexpr std::string_view persona_suffix_section = "deployment:persona-suffix";

// The centrally-owned placement tables, ported from the harness. Unknown
// names throw std::invalid_argument.
int section_order(std::string_view name);
int context_order(std::string_view name);

// Strict `{{variable}}` interpolation (exported for tests and renderers).
std::string interpolate(
	std::string_view text,
	std::map<std::string, std::optional<std::string>> const& variables,
	std::string_view kind,
	std::string_view name);

// Interpolate + join sections with a blank line; '' when all are empty.
std::string render_prompt(prompt_assembly const& assembly);

// The superseding runtime-context snapshot; '' when no context is active.
std::string render_context_snapshot(prompt_assembly const& assembly);

// The plugin descriptor: no dependencies, provides {"system-prompt",1}.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::system_prompt
