#pragma once

#include "araya/agent-loop/events.hpp"
#include "araya/agent-loop/inbox.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/task.hpp"
#include "araya/tools/tools.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The agent loop: drives one session through model turns and steps over the
// llm seam. Every request is derived from the session surface; every durable
// boundary (turn/start, step/start, request/header, tool/call, ...) is an
// ordinary session event, so the history replays without the loop.
//
// The service is the per-session driver. A session's pending input lives in
// the durable inbox projection (agent/inbox/spliced); `followup`, `steer`,
// and `inject` queue work there and `run` drives to quiescence. A driver
// claims its step's messages at the pre-step waterfall, so plugins can
// reject or rewrite them. Live lifecycle and inbox notifications go out on
// the engine bus, agent-scoped by the session's realm.
//
// A feature-replication of the deepseek-harness agent-loop core, trimmed to
// the v1 shape: sequential tool execution, no retry/maintenance phases.
//
// Prompt and tools come from the registries: the loop assembles the system
// prompt before each step and projects the rendered text into history (one
// effective system node), and executes tools through the tools service.
//
// Threading: strand-confined like the services it wraps - the driver methods
// must be called from the control strand.
namespace araya::agent {

// -- one run ---------------------------------------------------------------

struct run_options {
	std::string provider;
	std::string model;
	std::string reasoning_effort; // empty = model default
	std::optional<double> temperature;
	std::optional<std::uint64_t> max_tokens;
	araya::session::session_id session; // required
	// The user text this run opens with; appended directly (the one-shot
	// path - not queued in the durable inbox) so the system prompt folds
	// before it. Empty = no new user message.
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
// reference through the drive's chunk sink, so they are never copied into
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

// The non-durable execution parameters of a drive: the route, model knobs,
// cancellation, and the live observers. A waking call may supply these;
// empty route fields inherit the session's last logged request header, and
// the sinks attach to the drive the call starts.
struct drive_options {
	std::string provider;
	std::string model;
	std::string reasoning_effort;
	std::optional<double> temperature;
	std::optional<std::uint64_t> max_tokens;
	std::uint32_t max_steps = 16;
	std::stop_token stop;
	event_sink on_event;
	araya::llm::chunk_sink on_chunk;
};

// -- context producers -----------------------------------------------------
//
// A durable context contribution: on a turn's first step the driver asks
// each registered producer for one user message and places it before the
// claimed batch (and after the runtime-context snapshot). This is the seam
// the skill catalog and workspace instructions use instead of a prompt
// section, matching the harness's pre-step message injection.
struct context_producer_context {
	std::string provider;
	std::string model;
	std::string cwd;
};

struct context_producer {
	// A stable id: the message source tag and the dedup key.
	std::string name;
	// Returns the durable user message to contribute, or nullopt for
	// nothing. Producers are expected to be idempotent across turns; use
	// `injected_context` to append only when the rendered content changed.
	std::function<std::optional<boost::json::value>(
		araya::session::session const& session,
		context_producer_context const& context)>
		produce;
};

// The idempotent-context helper: builds the durable user message for
// `content`, tagged {kind:"plugin", plugin:producer}, or nullopt when
// `content` is empty or the session's surface already carries this
// producer's current content (so a restart or replay never duplicates it).
std::optional<boost::json::value>
injected_context(araya::session::session const& session, std::string_view producer, std::string_view content);

// -- the service -----------------------------------------------------------

class agent_service : public std::enable_shared_from_this<agent_service> {
public:
	agent_service(
		std::shared_ptr<araya::event_bus> bus,
		std::shared_ptr<araya::context> scope,
		boost::asio::any_io_executor executor,
		std::shared_ptr<araya::llm::llm_service> llm,
		std::shared_ptr<araya::session::session_store> store,
		std::shared_ptr<araya::system_prompt::system_prompt_service> prompts,
		std::shared_ptr<araya::tools::tools_service> tools,
		araya::session::projection_state<inbox_state> inbox,
		araya::session::projection_state<turn_state> turn_boundary);

	// -- lifecycle (the plugin wires these to the store's events) --

	// Registers a driver for a session (idempotent) and emits
	// 'agent/created'. `source` records why the lifecycle began.
	araya::task<void> ensure(araya::session::session const& session, boost::json::value source);
	// Removes a session's driver, cancelling it, and emits 'agent/disposed'.
	araya::task<void> dispose(araya::session::session_id const& session);
	// Teardown: cancels every driver without lifecycle notification (the
	// bus is winding down; mirrors the store's teardown discipline).
	void shutdown();

	// -- the durable inbox driver --

	// One-shot convenience: append `options.input` directly (not through
	// the inbox), drive the session to quiescence, and return the last
	// turn's outcome. Provider failures return as the outcome; program
	// errors throw.
	araya::task<run_outcome>
	run(run_options const& options, event_sink const& sink = {}, araya::llm::chunk_sink const& on_chunk = {});

	// Queue input durably. `followup`/`steer` wake an idle driver;
	// `inject` waits for the running turn's next step.
	araya::task<void>
	followup(araya::session::session_id const& session, boost::json::value message, drive_options options = {});
	araya::task<void>
	steer(araya::session::session_id const& session, boost::json::value message, drive_options options = {});
	araya::task<void> inject(araya::session::session_id const& session, boost::json::value message);

	// Registers a context producer owned by `caller` (removed at teardown).
	// Producers run in registration order on a turn's first step.
	araya::registration add_context_producer(araya::plugin_context& caller, context_producer producer);

	// Cancels the running drive; unless `keep_inbox`, clears pending input
	// (emitting 'agent/inbox/discarded' per message).
	void
	cancel(araya::session::session_id const& session, cancel_cause cause = cancel_cause::user, bool keep_inbox = false);

	agent_status status(araya::session::session_id const& session) const;

	// Awaits until no drive is running for the session.
	araya::task<void> when_idle(araya::session::session_id const& session);

	// The ids of every entered session at delegation depth zero.
	std::vector<araya::session::session_id> roots() const;

	// -- reads --

	inbox_state const* inbox(araya::session::session_id const& session) const;
	turn_state const* turn_boundary(araya::session::session_id const& session) const;
	run_outcome last_outcome(araya::session::session_id const& session) const;
	// The per-session realm scope agent events are dispatched under. The
	// pointer is owned by the driver registry and dies with the session.
	araya::context const* scope_of(araya::session::session_id const& session) const;

private:
	struct agent_state;
	using state_ptr = std::shared_ptr<agent_state>;

	state_ptr find(araya::session::session_id const& session) const;
	state_ptr require(araya::session::session_id const& session) const;

	void emit_status(state_ptr const& state, bool running);
	void resolve_route(state_ptr const& state, araya::session::session const& session);
	void merge_exec(state_ptr const& state, drive_options const& options);
	// Durably append one message to a queue and emit 'agent/inbox/inserted'.
	void enqueue(state_ptr const& state, inbox_target target, boost::json::value const& message);
	void clear_inbox(state_ptr const& state);

	void kick(state_ptr const& state);
	void on_drive_done(state_ptr const& state, std::exception_ptr ep);
	araya::task<void> drive(state_ptr state);
	araya::task<bool> turn(state_ptr const& state);
	araya::task<pre_step_msg> pre_step(
		state_ptr const& state,
		araya::session::session& session,
		std::uint64_t turn,
		std::uint64_t step,
		bool first_step,
		araya::system_prompt::prompt_assembly& assembly);
	araya::task<std::optional<run_status>> run_step(
		state_ptr const& state,
		araya::session::session& session,
		std::uint64_t turn,
		std::uint64_t step,
		araya::system_prompt::prompt_assembly const& prompt,
		std::vector<boost::json::value> const& messages,
		event_sink const& sink);
	araya::task<bool> execute_tools(
		state_ptr const& state,
		araya::session::session& session,
		std::uint64_t turn,
		std::uint64_t step,
		std::vector<araya::llm::tool_call_block> const& calls,
		event_sink const& sink);

	araya::system_prompt::prompt_assembly
	assemble_prompt(araya::session::session& session, drive_options const& options);
	boost::json::value
	build_header(drive_options const& options, araya::system_prompt::prompt_assembly const& prompt) const;
	araya::llm::generate_options build_generate(
		drive_options const& options,
		araya::session::session& session,
		araya::system_prompt::prompt_assembly const& prompt) const;
	void commit_system_prompt(araya::session::session& session, std::string const& rendered);
	void append_user_messages(araya::session::session& session, std::vector<boost::json::value> const& messages);
	void append_request_header(araya::session::session& session, boost::json::value const& header);
	void append_request_context(araya::session::session& session, drive_options const& options);

	std::shared_ptr<araya::event_bus> bus_;
	std::shared_ptr<araya::context> scope_;
	boost::asio::any_io_executor executor_;
	std::shared_ptr<araya::llm::llm_service> llm_;
	std::shared_ptr<araya::session::session_store> store_;
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts_;
	std::shared_ptr<araya::tools::tools_service> tools_;
	araya::session::projection_state<inbox_state> inbox_;
	araya::session::projection_state<turn_state> turn_boundary_;
	std::map<araya::session::session_id, state_ptr> states_;
	std::vector<std::pair<std::uint64_t, context_producer>> context_producers_;
	std::uint64_t next_context_producer_id_ = 1;
};

inline constexpr araya::service_key<agent_service> agent_key{"agent", 1};

// The plugin descriptor: requires `llm`, `sessions`, `system-prompt`, and
// `tools`; provides `agent`. It registers the built-in prompt variables
// (provider/model/cwd), the inbox and turnBoundary projections, and the
// session lifecycle listeners that create and dispose drivers.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::agent
