#pragma once

#include "araya/events.hpp"
#include "araya/session/session_types.hpp"

#include <memory>

namespace araya::session {

class session;

// 'session/created' (emit): an entered session was announced. Listeners
// receive the live session; they run fire-and-forget, so unlike the JS
// lineage there is no synchronous veto — creation validation happens in the
// store before announcement.
struct session_created_msg {
	std::shared_ptr<session> s;
};

// 'session/disposed' (emit): an announced session left the store (host
// dispose, owning-fiber unload, or store teardown).
struct session_disposed_msg {
	session_id id;
};

// 'session/event' (emit): the append firehose. Every event — including
// ignorable vocabulary — is published as it is appended, exactly as
// recorded. Persistence plugins subscribe here.
struct session_appended_msg {
	session_id id;
	session_event event;
};

// 'session/flush' (parallel): a durability barrier. Every listener runs and
// the caller awaits all of them; persistence plugins drain buffered events
// to durable storage here.
struct session_flush_msg {
	session_id id;
};

inline constexpr araya::event_key<session_created_msg, araya::dispatch_mode::emit> created_key{"session/created", 1};

inline constexpr araya::event_key<session_disposed_msg, araya::dispatch_mode::emit> disposed_key{"session/disposed", 1};

inline constexpr araya::event_key<session_appended_msg, araya::dispatch_mode::emit> appended_key{"session/event", 1};

inline constexpr araya::event_key<session_flush_msg, araya::dispatch_mode::parallel> flush_key{"session/flush", 1};

} // namespace araya::session
