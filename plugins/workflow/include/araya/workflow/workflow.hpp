#pragma once

#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/session/store.hpp"
#include "araya/subagents/subagents.hpp"
#include "araya/task.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

// The workflow engine seam: run a plain-JavaScript orchestration script that
// fans out subagents and returns the script's final JSON value. A
// feature-replication of the deepseek-harness `@deepseek-ai/dsh-workflow`
// seam, trimmed to the v1 shape: one out-of-process Node worker, a small
// length-prefixed JSON control channel, and the `agent`/`pipeline`/`parallel`/
// `phase`/`log`/`args` script surface. The worker owns JS orchestration; the
// host owns child delegation through the subagent seam.
//
// Threading: strand-confined like every service. run() must be called on the
// control strand; the worker's messages and the child runs are driven there.
namespace araya::workflow {

// Deployment knobs.
struct config {
	std::string node_executable = "node";
	// Maximum accepted control frame (and queued-write) size.
	std::size_t max_message_bytes = 4 * 1024 * 1024;
	// Agents the host runs at once; further calls queue.
	std::uint32_t max_concurrency = 4;
	// Ceiling on accepted `agent()` calls over one run.
	std::uint32_t max_total_agents = 64;
};

// What a caller asks a run to do. `meta`/`args` are plain JSON data.
struct start_request {
	std::string script;
	boost::json::value meta;					   // {name, description, whenToUse?, phases?}
	boost::json::value args;					   // null when absent
	std::string parent;							   // the parent session id (lineage)
	std::optional<std::string> provider;		   // subagent provider override
	std::optional<std::uint32_t> max_total_agents; // per-run ceiling override
	std::stop_token stop;
};

// A live progress notification (host-observed): run-start, log, phase,
// agent-start, agent-end, run-end.
struct progress {
	std::string kind;
	boost::json::value data;
};

using progress_sink = std::function<void(progress const&)>;

// The settled run: completed | cancelled | error. `value` is the script's
// return value (null for no return), meaningful only for `completed`.
struct result {
	std::string stop_reason;
	std::optional<boost::json::value> value;
	std::optional<std::string> error;
	std::uint64_t agents_started = 0;
};

// A live worker transport. The host drives one script through it. `send`
// queues one host->worker frame; `pump` streams worker->host frames to the
// callback and completes when the worker exits; `kill` force-terminates.
class worker {
public:
	virtual ~worker() = default;
	virtual void send(boost::json::value message) = 0;
	virtual araya::task<void> pump(std::function<void(boost::json::value)> on_message) = 0;
	virtual void kill() = 0;
};

using worker_factory = std::function<std::shared_ptr<worker>()>;

class workflow_service : public std::enable_shared_from_this<workflow_service> {
public:
	workflow_service(
		boost::asio::any_io_executor executor,
		std::shared_ptr<araya::session::session_store> store,
		std::shared_ptr<araya::subagents::subagents_service> subagents,
		config cfg);

	// Overrides the worker transport (tests use an in-process fake).
	void set_worker_factory(worker_factory factory);

	// Runs one script to settlement, reporting progress as it goes. A missing
	// parent session or invalid meta is a clean `error` result.
	araya::task<result> run(start_request request, progress_sink on_progress = {});

	// Kill live workers (teardown).
	void shutdown();

private:
	struct run_state;
	using state_ptr = std::shared_ptr<run_state>;
	using session_ptr = std::shared_ptr<araya::session::session>;

	// The real body, run confined to `executor_` (the control strand) so its
	// session appends dispatch on that strand regardless of the caller.
	araya::task<result> run_impl(start_request request, progress_sink on_progress);

	void handle_message(
		state_ptr const& state,
		session_ptr const& session,
		progress_sink const& on_progress,
		boost::json::value message);
	void handle_call(
		state_ptr const& state,
		session_ptr const& session,
		progress_sink const& on_progress,
		boost::json::value call);
	// Starts the next queued call when the run has capacity.
	void pump_calls(state_ptr const& state, session_ptr const& session, progress_sink const& on_progress);
	araya::task<void> finish_agent_call(
		state_ptr const& state,
		session_ptr const& session,
		progress_sink const& on_progress,
		boost::json::value call,
		araya::subagents::start_request request,
		std::uint64_t seq);
	void send_reply(state_ptr const& state, boost::json::value id, boost::json::value value);
	void send_error_reply(state_ptr const& state, boost::json::value id, std::string message);

	boost::asio::any_io_executor executor_;
	std::shared_ptr<araya::session::session_store> store_;
	std::shared_ptr<araya::subagents::subagents_service> subagents_;
	config config_;
	worker_factory factory_;
	std::uint64_t next_run_ = 1;
	std::vector<std::shared_ptr<worker>> live_workers_;
};

inline constexpr araya::service_key<workflow_service> workflow_key{"workflow", 1};

// The plugin descriptor: requires `sessions` and `subagents`; provides
// `workflow`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::workflow
