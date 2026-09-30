#pragma once

#include "araya/agent-loop/agent.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/task.hpp"
#include "araya/tools/tools.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// The subagent delegation seam: a named-provider registry plus the
// in-process orchestration of durable child agents. A feature-replication
// of the deepseek-harness `@deepseek-ai/dsh-subagent` seam, trimmed to the
// v1 shape: one in-process `spawn` provider, foreground one-shot runs, and
// continuable children that are ordinary driver sessions.
//
// A continuable child is a session driven by the agent service: its first
// turn is a `followup` carrying the task, later input is a `steer` while it
// runs and a `followup` once it is idle, and a settlement is appended to
// the parent whenever the child's driver goes idle. The service keeps only
// the provider registry, route/depth resolution, lineage, and the control
// surface - no private turn loop or pending queue.
//
// Threading: strand-confined like every service. The service captures the
// control-strand executor at apply time; the settlement delivery co_spawns
// there so its appends satisfy the session store's control-strand dispatch
// discipline.
namespace araya::subagents {

// Start-time features a provider supports. A request needing a capability
// the chosen provider lacks is rejected before any child exists.
struct capabilities {
	bool one_shot = true;
	bool continuable = true;
	bool depth_limit = true;
	bool agent_options = true;
	bool output_schema = true;
	// Whether a child starts from the parent's conversation context (the fork
	// provider) rather than fresh (spawn). Informational: the service does not
	// gate on it, but the tool layer can describe the inheritance.
	bool inherits_parent_context = false;
};

// What a caller asks for when delegating. `parent` is the calling session
// (lineage and inheritance source); `stop` is the caller's cancellation.
struct start_request {
	std::string prompt;
	std::string parent;
	std::string label;
	std::stop_token stop;
	// Optional child route overrides. Absent fields inherit the parent's
	// latest logged request header.
	std::optional<std::string> provider;
	std::optional<std::string> model;
	std::optional<std::string> reasoning_effort;
	// Optional delegation-depth cap for this call (the deployment's is the
	// ceiling on any call override).
	std::optional<std::uint32_t> max_depth;
	// Optional object-rooted JSON Schema. When set (one-shot runs only), the
	// child reports its final answer by calling a scoped `structured_output`
	// tool and the validated value lands in result.structure.
	std::optional<boost::json::value> output_schema;
};

// One settled (or rejected) delegation outcome.
struct result {
	bool is_error = false;
	// completed | max_tokens | aborted | error | blocked | running
	std::string stop_reason;
	std::string output;
	std::string error;
	// The child session id (set whenever a child was created).
	std::string child;
	// The validated structured result, set iff the request carried an
	// output_schema and the child recorded one.
	std::optional<boost::json::value> structure;
};

// Observe-only view of one continuable child.
struct child_info {
	std::string id;
	std::string parent;
	std::string label;
	std::string provider;
	bool running = false;
	std::size_t pending = 0;
	std::string stop_reason;
};

// A child-creation backend. Spawn opens a fresh session; a future fork
// provider seeds the parent's completed turns. The service owns turn
// orchestration; the provider owns only lineage.
class subagent_provider {
public:
	virtual ~subagent_provider() = default;

	virtual capabilities caps() const = 0;

	// Create (and enter) the child session for this request. `child_depth`
	// is the parent's delegation depth plus one.
	virtual std::shared_ptr<araya::session::session>
	create_child(start_request const& req, std::uint32_t child_depth) = 0;
};

class subagents_service : public std::enable_shared_from_this<subagents_service> {
public:
	subagents_service(
		boost::asio::any_io_executor executor,
		std::shared_ptr<araya::session::session_store> store,
		std::shared_ptr<araya::agent::agent_service> agent,
		std::shared_ptr<araya::tools::tools_service> tools,
		std::shared_ptr<araya::system_prompt::system_prompt_service> prompts,
		std::uint32_t max_depth);

	// The plugin hands the service a factory for fresh plugin-context views
	// over its own activation; the service uses it to register the scoped
	// structured-output tool/section for a child at run time.
	void set_context_factory(std::function<araya::plugin_context()> factory);

	// Registers a provider under `name`; owned by `caller`.
	araya::registration
	register_provider(araya::plugin_context& caller, std::string name, std::shared_ptr<subagent_provider> provider);

	std::vector<std::string> provider_names() const;
	std::shared_ptr<subagent_provider> find_provider(std::string_view name) const;

	// One-shot: create the child, run one turn to completion, return its
	// final output.
	araya::task<result> run(std::string_view provider, start_request req);

	// Continuable: create the child, start its first turn in the
	// background, and return the child id immediately. Each turn's outcome
	// is appended to the parent session when it settles.
	araya::task<result> start_continuable(std::string_view provider, start_request req);

	// Control tools.
	araya::task<result> send_message(std::string const& child, std::string text);
	bool interrupt(std::string const& child);
	std::vector<child_info> list_children(std::optional<std::string> const& parent = {}) const;

	// A child's driver went idle: deliver its settlement to the parent.
	// Wired by the plugin to the agent's idle transition.
	void on_agent_idle(araya::session::session_id const& child);

	// Disposes every child this service created (teardown).
	void dispose_children();

private:
	struct child_state {
		std::string id;
		std::string parent;
		std::string label;
		std::string backend;		// the seam provider name (spawn)
		std::string route_provider; // the child's LLM provider
		std::string model;
		std::string reasoning_effort;
		std::stop_token parent_stop;
	};

	// Resolves route/depth, creates the child, and records it as a
	// continuable child. Throws on unknown provider or depth overrun.
	std::shared_ptr<araya::session::session>
	create_child(std::string_view provider_name, start_request const& req, child_state& state);

	// Builds the settlement notice from a child's last outcome and written
	// it through the shared inbox verbs.
	araya::task<void> deliver_settlement(std::string child_id);
	std::optional<std::string> inherit_route(std::string const& parent, std::string_view key) const;

	boost::asio::any_io_executor executor_;
	std::shared_ptr<araya::session::session_store> store_;
	std::shared_ptr<araya::agent::agent_service> agent_;
	std::shared_ptr<araya::tools::tools_service> tools_;
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts_;
	std::function<araya::plugin_context()> context_factory_;
	std::uint32_t max_depth_;
	std::map<std::string, std::shared_ptr<subagent_provider>> providers_;
	std::map<std::string, child_state> children_;
};

// The built-in in-process spawn provider: a fresh child session with no
// seed, inheriting the parent's cwd and preset.
std::shared_ptr<subagent_provider> make_spawn_provider(std::shared_ptr<araya::session::session_store> store);

inline constexpr araya::service_key<subagents_service> subagents_key{"subagents", 1};

// The plugin descriptor: requires `sessions` and `agent`; provides
// `subagents`, and registers the built-in `spawn` provider.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::subagents
