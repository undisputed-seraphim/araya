#pragma once

#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/task.hpp"
#include "araya/tools/tools.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The agent loop: drives one session through model steps over the llm
// seam. Every request is derived from the session surface; every durable
// boundary (turn/start, step/start, request/header, tool/call, ...) is an
// ordinary session event, so the history replays without the loop. A
// feature-replication of the deepseek-harness agent-loop core, trimmed to
// the v1 shape: sequential tool execution, no inbox/steering, no retry
// (that arc consumes the observer events).
//
// Prompt and tools come from the registries: the loop assembles the
// system prompt from the system-prompt service before each step and
// projects the rendered text into history (one effective system node),
// and executes tools through the tools service.
//
// Threading: strand-confined like the services it wraps - run() and the
// registration methods must be called from the control strand.
namespace araya::agent {

// -- one run ---------------------------------------------------------------

struct run_options {
	std::string provider;
	std::string model;
	std::string reasoning_effort; // empty = model default
	std::optional<double> temperature;
	std::optional<std::uint64_t> max_tokens;
	araya::session::session_id session; // required
	// The user text this run opens with; the loop appends the
	// user/message itself so the system prompt folds before it (the
	// harness's step order). Empty = no new user message.
	std::string input;
	std::uint32_t max_steps = 16; // hard cap on steps per turn
	std::stop_token stop;
};

enum class run_status : std::uint8_t {
	completed,	// the model stopped calling tools
	max_tokens, // the model hit its output ceiling
	aborted,	// the stop token fired
	error,		// the provider failed (failure carries it)
	blocked,	// the step cap was reached mid-tool-loop
};

// The stable display name for a run status (also the turn/end reason).
char const* run_status_name(run_status status) noexcept;

struct run_outcome {
	run_status status = run_status::completed;
	// The provider failure, set iff status is error.
	std::optional<araya::llm::llm_failure> failure;
	std::uint64_t turn = 0;
};

// -- live notifications ----------------------------------------------------

// The transient-streaming seam: durable history is the session log; these
// events are for the surface. Model chunks are forwarded separately, by
// reference through the run's chunk sink, so they are never copied into
// this variant.
struct turn_event {
	std::uint64_t turn;
};

struct step_event {
	std::uint64_t turn;
	std::uint64_t step;
};

struct tool_call_event {
	std::uint64_t turn;
	std::uint64_t step;
	std::string call_id;
	std::string name;
};

struct tool_done_event {
	std::uint64_t turn;
	std::uint64_t step;
	std::string call_id;
	std::string name;
	bool is_error = false;
};

struct run_finish_event {
	std::uint64_t turn;
	run_status status;
};

using agent_event = std::variant<turn_event, step_event, tool_call_event, tool_done_event, run_finish_event>;

using event_sink = std::function<void(agent_event const&)>;

// -- the service -----------------------------------------------------------

class agent_service {
public:
	agent_service(
		std::shared_ptr<araya::llm::llm_service> llm,
		std::shared_ptr<araya::session::session_store> store,
		std::shared_ptr<araya::system_prompt::system_prompt_service> prompts,
		std::shared_ptr<araya::tools::tools_service> tools);

	// Drives one turn of `options.session`: steps until the model stops
	// calling tools (or the run ends for another reason). Lifecycle events
	// go to `sink`; raw model chunks (deltas, block ends, usage) go to
	// `on_chunk` by reference, uncopied. Throws only for program errors
	// (unknown session, missing provider/model); provider failures return
	// as run_outcome.
	araya::task<run_outcome>
	run(run_options const& options, event_sink const& sink = {}, araya::llm::chunk_sink const& on_chunk = {});

private:
	araya::system_prompt::prompt_assembly assemble_prompt(araya::session::session& session, run_options const& options);

	boost::json::value
	build_header(run_options const& options, araya::system_prompt::prompt_assembly const& prompt) const;
	araya::llm::generate_options build_generate(
		run_options const& options,
		araya::session::session& session,
		araya::system_prompt::prompt_assembly const& prompt) const;
	// Projects the rendered prompt into history as one effective system
	// node: append the first time, splice-replace the previous defining
	// event when the text changes.
	void commit_system_prompt(araya::session::session& session, std::string const& rendered);
	void append_runtime_context(araya::session::session& session, araya::system_prompt::prompt_assembly const& prompt);
	void append_request_header(araya::session::session& session, boost::json::value const& header);
	void append_request_context(araya::session::session& session, run_options const& options);
	// The pre-stream half of one step: announce it, assemble and commit the
	// prompt, append the run's user input on the first step, and record the
	// request header/context. Returns the assembly the step generates from.
	araya::system_prompt::prompt_assembly open_step(
		araya::session::session& session,
		run_options const& options,
		std::uint64_t turn,
		std::uint64_t step,
		event_sink const& sink);
	araya::task<bool> execute_tools(
		araya::session::session& session,
		run_options const& options,
		std::uint64_t turn,
		std::uint64_t step,
		std::vector<araya::llm::tool_call_block> const& calls,
		event_sink const& sink);

	std::shared_ptr<araya::llm::llm_service> llm_;
	std::shared_ptr<araya::session::session_store> store_;
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts_;
	std::shared_ptr<araya::tools::tools_service> tools_;
};

inline constexpr araya::service_key<agent_service> agent_key{"agent", 1};

// The plugin descriptor: requires `llm`, `sessions`, `system-prompt`, and
// `tools`; provides `agent`. It also registers the built-in prompt
// variables (provider/model/cwd).
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::agent
