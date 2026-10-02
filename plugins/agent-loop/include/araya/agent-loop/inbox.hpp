#pragma once

#include "araya/session/projection.hpp"
#include "araya/session/session_types.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The durable agent inbox: the two ordered queues of pending messages that
// a long-lived agent driver consumes. The queues are not held in memory
// alongside history - they are a projection over the session log, so an
// inbox restores and forks exactly like the surface does. Every mutation is
// one 'agent/inbox/spliced' event carrying a toSpliced splice; the fold
// replays them in sequence.
//
// Claim discipline (mirrored by the future driver): a turn's first step
// claims one next-turn message plus every next-step message; later steps
// claim next-step only. A claimed message is removed from its queue by a
// pure-delete splice, so the log records the claim.
namespace araya::agent {

// Which queue a splice targets. next-turn carries a new turn's opening
// input; next-step carries mid-turn continuations (tool notices, goal
// rounds, steering) that the currently running turn must not close over.
enum class inbox_target : std::uint8_t {
	next_turn,
	next_step,
};

// The event type the inbox owns. It is ordinary ignorable vocabulary to
// every reader without the agent-loop projection registered, and is never
// surface-relevant (an inbox entry only reaches the model once a turn
// claims and commits it as a user message).
inline constexpr std::string_view k_inbox_spliced_event = "agent/inbox/spliced";

// The fold state for one session: the two pending-message queues, each an
// ordered list of full message payloads ({id, role:"user", content, source}).
struct inbox_state {
	std::vector<boost::json::value> next_turn;
	std::vector<boost::json::value> next_step;
};

// The 'agent/inbox/spliced' payload: a splice over one queue with
// Array.prototype.toSpliced coordinates. `start` is clamped to [0, size]
// (a negative start counts from the end), `removed_count` to
// [0, size - start], and `inserted` are full message payloads inserted at
// the (clamped) start in order. `outcome` is informational provenance for
// discards/cancels; the fold ignores it.
boost::json::value inbox_splice_data(
	inbox_target target,
	std::int64_t start,
	std::int64_t removed_count,
	boost::json::array inserted = {},
	std::string_view outcome = {});

// Appends/prepends one message. Append uses the toSpliced clamp (an index
// at or past the end inserts at the end), so no queue length is needed.
boost::json::value inbox_append_data(inbox_target target, boost::json::value const& message);
boost::json::value inbox_prepend_data(inbox_target target, boost::json::value const& message);

// Removes the first `count` messages of a queue (the claim).
boost::json::value inbox_claim_data(inbox_target target, std::int64_t count);

// The normalized result of one mutation: the exact splice payload to
// append (coordinates already clamped, `outcome` resolved) plus the
// messages it removed. `noop` is true when the splice changes nothing, in
// which case no event is written.
struct inbox_mutation {
	boost::json::value data;
	std::vector<boost::json::value> removed;
	bool noop = false;
};

// Normalizes a splice the way the durable fold will read it (the harness's
// mutate): clamps `start`/`delete_count` to the current queue, returns the
// removed messages, omits `removedCount` when zero, and marks
// `outcome:"canceled"` only when `discard` is set and something was
// removed. A no-op yields `noop` and no payload.
inbox_mutation make_inbox_splice(
	inbox_state const& state,
	inbox_target target,
	std::int64_t start,
	std::int64_t delete_count,
	boost::json::array inserted,
	bool discard);

// Folds one 'agent/inbox/spliced' payload into the state. Malformed payloads
// are a no-op. Message ids are unique across both queues: an inserted entry
// with no id, or an id already present, is dropped rather than duplicated,
// so a misbehaving producer cannot corrupt a replay.
void apply_inbox_splice(inbox_state& state, boost::json::value const& data);

// The store-driven projection for the durable inbox.
araya::session::event_projection<inbox_state> inbox_projection();

// The message id a queue entry carries ("" when absent or malformed).
std::string inbox_message_id(boost::json::value const& message);

// The id without allocating; empty when absent or not a string.
std::string_view inbox_message_id_view(boost::json::value const& message);

} // namespace araya::agent
