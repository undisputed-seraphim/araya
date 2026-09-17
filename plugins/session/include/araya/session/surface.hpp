#pragma once

#include "araya/session/session_types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace araya::session {

// The ordered surface over a session's event log: the fold of every
// surface-relevant event into the LLM-facing message history. It is a view
// of derived state, rebuilt incrementally as events are appended.
class session_surface {
public:
    std::vector<session_message> const& messages() const noexcept {
        return messages_;
    }

    void push(session_message msg) { messages_.push_back(std::move(msg)); }

private:
    std::vector<session_message> messages_;
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

// Folds one event into an optional surface message: registered projections
// win over the built-in fold; non-surface vocabulary yields nullopt.
std::optional<session_message> fold_event(
    session_event const& ev,
    std::vector<message_projection> const& projections);

}  // namespace araya::session
