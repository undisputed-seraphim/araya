#pragma once

#include "araya/session/session_types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace araya::session {

// The ordered surface over a session's event log: the fold of every
// surface-relevant event into the LLM-facing message history. It is a view
// of derived state, rebuilt incrementally as events are appended. Each
// message remembers the sequence of the event it derived from, so a
// 'surface/replace' event can splice out a span of history (the
// compaction story) by its source sequences.
class session_surface {
public:
	std::vector<session_message> const& messages() const noexcept { return messages_; }

	// Folds one appended event's message into the surface.
	void push(session_seq source_seq, session_message msg);

	// Splicing: erases every message whose source event falls in
	// [start, end), then appends the replacement (if any) at the end -
	// the replacement speaks for the whole removed span, in the position
	// of the event that removed it.
	void replace(session_seq start, session_seq end, session_seq source_seq, std::optional<session_message> msg);

private:
	std::vector<session_message> messages_;
	std::vector<session_seq> source_seqs_;
};

// What folding one event does to the surface. Registered projections
// return append plans (they own their message); the built-in fold turns
// the four message types into appends and 'surface/replace' into a
// splice.
struct surface_plan {
	enum class kind : std::uint8_t {
		none,
		append,
		replace,
	};

	kind action = kind::none;
	// The replaced span (replace only): [start, end), both inclusive of
	// the messages they bound.
	session_seq start = 0;
	session_seq end = 0;
	std::optional<session_message> message;
};

// A plugin-owned interpreter for one event type. Registering a projection
// makes that type surface-relevant (no longer ignorable): its events fold
// through the projection instead of the built-in fold. The contribution is
// a tracked effect on the registering fiber, so projections vanish when
// their owning plugin unloads.
struct message_projection {
	std::string event_type;
	std::function<std::optional<session_message>(session_event const&)> fold;
};

// Folds one event into a surface plan: registered projections win over the
// built-in fold; non-surface vocabulary yields a no-op plan.
surface_plan fold_event(session_event const& ev, std::vector<message_projection> const& projections);

} // namespace araya::session
