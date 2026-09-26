#pragma once

#include "araya/effects.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/task.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// The background-job registry contract: ids, owner-scoped access, lifecycle
// state, output reads, waits, cancellation, and completion listeners. A
// feature-replication of the deepseek-harness `@deepseek-ai/dsh-jobs` seam,
// trimmed to the in-process shape.
//
// A producer (e.g. bash `run_in_background`) registers long-running work with
// a kind and a one-line label; the registry issues a `<kind>-N` id and owns
// identity and lifecycle while the producer owns execution resources. Access
// is fenced by the owning session id: ids are predictable, so authorization -
// not secrecy - is the boundary. Settlement is first-wins, and a completion
// listener (the model-facing notice in `tool-jobs`) runs after the record is
// committed.
//
// Divergences from the harness (recorded): the owner is a session id, not a
// live Agent; there are no scope-relative controller/listener layers (a single
// root scope); nothing survives the process.
namespace araya::jobs {

// A registry-issued id: `<kind>-<n>`.
using job_id = std::string;

// Task lifecycle: `running`, optionally `stopping`, then exactly one terminal
// status.
enum class job_status : std::uint8_t {
	running,
	stopping,
	completed,
	killed,
	failed,
};

char const* job_status_name(job_status status) noexcept;

bool is_terminal(job_status status) noexcept;

// The terminal result supplied by a producer.
struct job_outcome {
	// Must be a terminal status; a non-terminal value is coerced to `failed`.
	job_status status = job_status::completed;
	// Kind-specific detail rendered into status lines ("exit code: 3").
	std::string detail;
	// Final output for jobs without a `read_output` handle; stream jobs leave
	// it unset.
	std::string output;
};

// A read-only projection of one job (a fresh value per call, never live
// registry state).
struct job_snapshot {
	job_id id;
	std::string kind;
	std::string label;
	std::optional<std::size_t> output_limit_bytes;
	// The owner session id, or nullopt for an unowned job.
	std::optional<std::string> owner_session;
	job_status status = job_status::running;
	std::optional<std::string> detail;
	std::int64_t started_at_ms = 0;
	std::optional<std::int64_t> finished_at_ms;
	// True once a kill, read, wait, or teardown has committed to report the
	// terminal state; completion listeners suppress redundant notices.
	bool reported = false;
};

// Output and post-read state returned by read().
struct job_read {
	// Stream jobs: the consuming delta since the previous read. Final-output
	// jobs: empty while live, the terminal outcome once settled.
	std::string text;
	job_snapshot snapshot;
};

// The producer's control surface, returned synchronously by job_start::run.
struct job_handle {
	// Request termination: synchronous, idempotent, and eventually settles the
	// job through the settle callback.
	std::function<void(std::string const& reason)> cancel;
	// Consume output produced since the previous call. Absence marks a
	// final-output-only job.
	std::function<std::string()> read_output;
};

// Producer declaration passed to start(). The registry preflights access and
// cleanup before invoking `run`; `run` starts the work, stashes `settle`, and
// returns the control handle. Calling `settle` more than once is ignored
// (first-wins).
struct job_start {
	std::string kind;
	std::string label;
	// Empty optional creates an unowned job, open to any caller.
	std::optional<std::string> owner_session;
	std::optional<std::size_t> output_limit_bytes;
	std::function<job_handle(std::function<void(job_outcome)> settle)> run;
};

// A completion callback: the settled snapshot and the job's owner session
// (nullopt for an unowned job).
using job_done_listener =
	std::function<void(job_snapshot const& snapshot, std::optional<std::string> const& owner_session)>;

// An observer of a change to what list(owner) returns for one owner.
using jobs_changed_listener = std::function<void(std::optional<std::string> const& owner_session)>;

class jobs_service {
public:
	virtual ~jobs_service() = default;

	// Preflight access, validation, owner cleanup, and admission before
	// starting and registering work. A throwing starter leaves nothing
	// registered. Returns the registry-issued id.
	virtual job_id start(job_start spec) = 0;

	// Owned and unowned jobs visible to a caller, in registration order.
	// `owner_session` nullopt sees only unowned jobs.
	virtual std::vector<job_snapshot> list(std::optional<std::string> const& owner_session) const = 0;

	// A non-consuming snapshot. Throws for an unknown or foreign job.
	virtual job_snapshot get(job_id const& id, std::optional<std::string> const& owner_session) const = 0;

	// Read the next stream delta, or the idempotent final output after
	// settlement. A terminal read marks the job reported. Throws for an
	// unknown or foreign job.
	virtual job_read read(job_id const& id, std::optional<std::string> const& owner_session) = 0;

	enum class kill_result {
		requested,
		already_finished,
	};

	// Request cancellation. A producer throw propagates without changing job
	// state. Throws for an unknown or foreign job.
	virtual kill_result
	kill(job_id const& id, std::optional<std::string> const& owner_session, std::string const& reason) = 0;

	// Wait for settlement or timeout without cancelling the job. A timeout or
	// caller stop returns the current snapshot. Throws for an unknown or
	// foreign job.
	virtual araya::task<job_snapshot> wait(
		job_id id,
		std::uint64_t timeout_ms,
		std::optional<std::string> const& owner_session,
		std::stop_token stop) = 0;

	// Register a completion listener owned by `caller` (removed at teardown or
	// early via the returned registration).
	virtual araya::registration on_job_done(araya::plugin_context& caller, job_done_listener listener) = 0;

	// Register a visible-set observer owned by `caller`.
	virtual araya::registration on_jobs_changed(araya::plugin_context& caller, jobs_changed_listener listener) = 0;

	// Attach an effect-scoped controller; start() refuses while none is
	// attached, so a producer cannot start work its owner cannot collect or
	// stop.
	virtual araya::registration attach_controller(araya::plugin_context& caller, std::string name) = 0;
};

inline constexpr araya::service_key<jobs_service> jobs_key{"jobs", 1};

// The plugin descriptor: requires `sessions`; provides `jobs`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::jobs
