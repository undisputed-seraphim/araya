#pragma once

#include "araya/events.hpp"
#include "araya/plugin.hpp"
#include "araya/service.hpp"
#include "araya/session/projection.hpp"
#include "araya/session/session_types.hpp"
#include "araya/session/store.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The same-session goal domain: event-sourced durable state, compare-and-set
// mutations, and process-local continuation activation. A feature-
// replication of the deepseek-harness `@deepseek-ai/dsh-goal`, trimmed to the
// service, its panel projection, and the `goal/changed` notification (no
// remote/wire surface, no invariant companion).
//
// Durable state lives entirely in the session log: every non-clear mutation
// appends one `goal/change` carrying the complete post-mutation snapshot;
// a clear appends a revisioned tombstone. The `goal` projection replays them
// (and advances the admitted-round counter from goal-sourced user messages),
// so the service restores and forks with the session.
namespace araya::goal {

// The durable continuation phase.
enum class goal_phase : std::uint8_t {
	active,
	paused,
	blocked,
	complete,
};

// Whether this live process may automatically continue an active goal.
enum class goal_activation : std::uint8_t {
	armed,
	disarmed,
};

char const* goal_phase_name(goal_phase phase) noexcept;
char const* goal_activation_name(goal_activation activation) noexcept;

// A machine-routable and human-readable blocked explanation.
struct goal_block_reason {
	std::string code;
	std::string message;
};

// One full durable goal revision.
struct goal_snapshot {
	std::string id;
	std::uint64_t revision = 1;
	std::string objective;
	goal_phase phase = goal_phase::active;
	std::optional<goal_block_reason> blocked_reason;
	std::uint64_t max_goal_rounds = 256;
};

// Compare-and-set identity for one exact revision.
struct goal_ref {
	std::string id;
	std::uint64_t revision = 0;
};

// A detached live view: the snapshot plus replay counters and activation.
struct goal_view {
	std::string id;
	std::uint64_t revision = 1;
	std::string objective;
	goal_phase phase = goal_phase::active;
	std::optional<goal_block_reason> blocked_reason;
	std::uint64_t max_goal_rounds = 256;
	std::uint64_t rounds_started = 0;
	std::int64_t created_at = 0;
	std::int64_t updated_at = 0;
	goal_activation activation = goal_activation::disarmed;
};

// Stable rejection codes, mirroring the harness vocabulary.
enum class goal_error_code : std::uint8_t {
	agent_not_live,
	not_found,
	already_exists,
	stale_revision,
	invalid_objective,
	invalid_max_rounds,
	invalid_block_reason,
	invalid_edit,
	invalid_transition,
};

// Reserved API: maps a rejection code to its stable name; no in-tree caller
// yet (the model-facing tool serializes the code verbatim).
[[maybe_unused]] char const* goal_error_name(goal_error_code code) noexcept;

// The domain boundary's rejection error.
class goal_error : public std::runtime_error {
public:
	goal_error(std::string message, goal_error_code code);
	goal_error_code code;
};

// The `goal` projection's state: the current goal (absent before create and
// after clear), its admitted-round counter, and a captured replay failure.
struct goal_projection_state {
	std::optional<goal_snapshot> current;
	std::uint64_t rounds_started = 0;
	std::int64_t created_at = 0;
	std::int64_t updated_at = 0;
	std::optional<goal_ref> last_ref;
	std::vector<std::string> seen_ids;
	std::optional<std::string> failure;
};

// The event type goal mutations are recorded under.
inline constexpr std::string_view k_goal_change_event = "goal/change";

// The store-driven projection for durable goal facts.
araya::session::event_projection<goal_projection_state> goal_projection();

// 'goal/changed' (emit): one durable goal mutation committed. Listener
// failures are contained. The payload carries the owning session for
// filtering; the goal view is absent for a clear tombstone.
struct goal_changed_msg {
	araya::session::session_id session;
	std::string operation;
	goal_ref ref;
	std::optional<goal_view> goal;
};

inline constexpr araya::event_key<goal_changed_msg, araya::dispatch_mode::emit> goal_changed_key{"goal/changed", 1};

// The goal service: mutations are compare-and-set against the current
// revision and append one durable `goal/change` event; reads come from the
// projection. Threading: strand-confined like every service.
class goal_service {
public:
	goal_service(
		std::shared_ptr<araya::event_bus> bus,
		std::shared_ptr<araya::session::session_store> store,
		araya::session::projection_state<goal_projection_state> projection,
		std::uint64_t default_max_goal_rounds = 256);

	// Create and arm a goal. A completed goal may be replaced; any other
	// current phase must be cleared or resumed instead.
	goal_view create(
		araya::session::session_id const& session,
		std::string objective,
		std::optional<std::uint64_t> max_rounds = {});
	// Edit objective and/or round cap without changing phase.
	goal_view edit(
		araya::session::session_id const& session,
		goal_ref const& ref,
		std::optional<std::string> objective,
		std::optional<std::uint64_t> max_rounds);
	goal_view pause(araya::session::session_id const& session, goal_ref const& ref);
	goal_view resume(araya::session::session_id const& session, goal_ref const& ref);
	goal_view complete(araya::session::session_id const& session, goal_ref const& ref);
	goal_view block(araya::session::session_id const& session, goal_ref const& ref, goal_block_reason reason);
	// Clear the current goal, retaining a tombstone whose revision is one
	// past the cleared snapshot.
	goal_ref clear(araya::session::session_id const& session, goal_ref const& ref);

	// Read the current goal for a live session, or nullopt.
	std::optional<goal_view> get(araya::session::session_id const& session) const;
	// Remove process-local continuation authority without touching phase.
	std::optional<goal_view> disarm(araya::session::session_id const& session);
	goal_activation activation(araya::session::session_id const& session) const;
	// Drop all process-local state for a disposed session. Reserved API: the
	// session/disposed hook is not yet wired to call this.
	[[maybe_unused]] void forget(araya::session::session_id const& session);

	// Reserved API: the goal view is exposed through get(); no caller needs
	// the raw projection state externally yet.
	[[maybe_unused]] goal_projection_state const* state_of(araya::session::session_id const& session) const;

private:
	goal_view commit_snapshot(
		araya::session::session_id const& session,
		std::string_view operation,
		goal_snapshot const& goal,
		std::uint64_t rounds_started,
		std::int64_t created_at,
		std::int64_t updated_at,
		goal_activation activation);
	goal_view commit_current(
		araya::session::session_id const& session,
		std::string_view operation,
		goal_snapshot const& goal,
		goal_activation activation);
	void
	commit_change(araya::session::session_id const& session, boost::json::value change, goal_activation activation);
	goal_projection_state const& expect_state(araya::session::session_id const& session) const;
	void expect_current(goal_projection_state const& state, goal_ref const& ref) const;
	// The CAS prologue shared by every ref-targeted mutator: the session is
	// live, its projection is healthy, and `ref` names the current revision.
	goal_projection_state const&
	expect_current_goal(araya::session::session_id const& session, goal_ref const& ref) const;
	goal_activation activation_of(araya::session::session_id const& session) const;
	void set_activation(araya::session::session_id const& session, goal_activation activation);
	goal_view view_of(goal_projection_state const& state, goal_activation activation) const;
	goal_view ensure_session(araya::session::session_id const& session) const;

	std::shared_ptr<araya::event_bus> bus_;
	std::shared_ptr<araya::session::session_store> store_;
	araya::session::projection_state<goal_projection_state> projection_;
	std::uint64_t default_max_goal_rounds_;
	std::map<araya::session::session_id, goal_activation> activations_;
};

inline constexpr araya::service_key<goal_service> goals_key{"goals", 1};

// The plugin descriptor: requires `sessions`; provides `goals`. Registers
// the `goal` projection.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::goal
