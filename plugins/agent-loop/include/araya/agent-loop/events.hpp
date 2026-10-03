#pragma once

#include "araya/events.hpp"
#include "araya/session/projection.hpp"
#include "araya/session/session_types.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <optional>
#include <stop_token>
#include <vector>

// The agent-loop event vocabulary: the live, agent-scoped notifications a
// session's driver emits, plus the pre-step extension point. Everything
// here is plugin-layer vocabulary on the engine's existing bus; none of it
// is durable (the inbox's 'agent/inbox/spliced' is the durable half, in
// inbox.hpp). Agent-scoped dispatch tags each event with the session's
// realm scope, so a listener can subscribe to one agent.
namespace araya::agent {

// An agent's lifecycle state. `idle` means no driver is active; `running`
// spans from a waking input through the drain of its turns.
enum class agent_status : std::uint8_t {
	idle,
	running,
};

// Why a driver was cancelled.
enum class cancel_cause : std::uint8_t {
	user,
	parent,
	disposed,
};

// 'agent/created' (serial): a session acquired a live driver. `source`
// records why the lifecycle began ({kind:"startup"} for a fresh session,
// {kind:"resume"} for a seeded restore).
struct agent_created_msg {
	araya::session::session_id session;
	boost::json::value source;
};

// 'agent/disposed' (serial): a session's driver left the registry.
struct agent_disposed_msg {
	araya::session::session_id session;
};

// 'agent/status' (emit): emitted only on an idle/running transition.
struct agent_status_msg {
	araya::session::session_id session;
	agent_status status = agent_status::idle;
};

// 'agent/inbox/inserted' (emit): one message entered a pending queue.
struct agent_inbox_inserted_msg {
	araya::session::session_id session;
	boost::json::value message;
};

// 'agent/inbox/claimed' (emit): one message left the inbox inside its turn.
struct agent_inbox_claimed_msg {
	araya::session::session_id session;
	boost::json::value message;
	std::uint64_t turn = 0;
};

// 'agent/inbox/discarded' (emit): one message was removed without a step.
struct agent_inbox_discarded_msg {
	araya::session::session_id session;
	boost::json::value message;
};

// 'agent/turn-stopping' (serial): a step ended its turn and the turn is
// about to close; a listener may inject next-step work to keep it open.
struct agent_turn_stopping_msg {
	araya::session::session_id session;
	std::uint64_t turn = 0;
	std::stop_token stop;
};

// 'agent/pre-step' (waterfall): the proposed messages for a step, which is
// also the decision - our waterfall returns its final message, so a
// listener transforms this struct and passes it on. The default proposal is
// {reject=false, messages=[runtime-context?, claimed...]}. Setting
// `reject` ends the turn `blocked` without a step; an empty `messages` on a
// turn's first step ends it `completed` without a model call.
struct pre_step_msg {
	araya::session::session_id session;
	std::uint64_t turn = 0;
	std::uint64_t step = 0;
	std::stop_token stop;
	bool reject = false;
	std::vector<boost::json::value> messages;
	// A listener may set this on the turn's first step to start a fresh
	// request series: the loop then normalizes the system prompt and does not
	// reuse the cached conversation prefix.
	bool starts_request_series = false;
};

inline constexpr araya::event_key<agent_created_msg, araya::dispatch_mode::serial> agent_created_key{
	"agent/created",
	1};
inline constexpr araya::event_key<agent_disposed_msg, araya::dispatch_mode::serial> agent_disposed_key{
	"agent/disposed",
	1};
inline constexpr araya::event_key<agent_status_msg, araya::dispatch_mode::emit> agent_status_key{"agent/status", 1};
inline constexpr araya::event_key<agent_inbox_inserted_msg, araya::dispatch_mode::emit> agent_inbox_inserted_key{
	"agent/inbox/inserted",
	1};
inline constexpr araya::event_key<agent_inbox_claimed_msg, araya::dispatch_mode::emit> agent_inbox_claimed_key{
	"agent/inbox/claimed",
	1};
inline constexpr araya::event_key<agent_inbox_discarded_msg, araya::dispatch_mode::emit> agent_inbox_discarded_key{
	"agent/inbox/discarded",
	1};
inline constexpr araya::event_key<agent_turn_stopping_msg, araya::dispatch_mode::serial> agent_turn_stopping_key{
	"agent/turn-stopping",
	1};
inline constexpr araya::event_key<pre_step_msg, araya::dispatch_mode::waterfall> pre_step_key{"agent/pre-step", 1};

// -- turnBoundary ----------------------------------------------------------
//
// The fold a listener reads to learn a session's open/last turn and step
// boundary facts (the harness's `turnBoundary` projection). It restores and
// forks with the log like any typed projection.
struct step_boundary {
	bool start = true;
	std::uint64_t seq = 0;
};

struct turn_state {
	std::optional<std::uint64_t> open_turn_start_seq;
	std::optional<std::uint64_t> last_step_start_seq;
	std::optional<step_boundary> last_step_boundary;
	std::uint64_t last_turn = 0;
};

araya::session::event_projection<turn_state> turn_boundary_projection();

} // namespace araya::agent
