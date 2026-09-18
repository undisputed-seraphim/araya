#pragma once

#include <boost/json/value.hpp>

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace araya::session {

// The single monotonic format version stamped into every session header. We
// start at 1: the JS lineage's v1-v3 migration history is load-bearing
// storage archaeology we do not inherit. Bump only when an older runtime
// would read a new log with wrong semantics.
inline constexpr std::uint32_t SESSION_FORMAT_VERSION = 1;

// Session identity: a plain string minted by the store ("session-<n>") or
// supplied by the host (agent loops key sessions off their own ids).
struct session_id {
	std::string value;

	friend auto operator<=>(session_id const&, session_id const&) = default;
};

// Sequence number of one existing event in a session log; the log offset may
// equal the event count (a gap, prefix length, or read offset).
using session_seq = std::uint64_t;
using session_log_offset = std::uint64_t;

enum class session_origin : std::uint8_t { none, subagent };

// Immutable validated storage metadata, kept outside the conversation event
// log: identity, lineage, and presentation facts. The log itself is
// replayable history; the header is a storage concern.
struct session_header {
	std::uint32_t version = SESSION_FORMAT_VERSION;
	session_id id;
	// Unix epoch milliseconds when the session was created.
	std::int64_t created_at = 0;
	std::optional<std::string> cwd;
	// The session this one was forked from (seed lineage), if any.
	std::optional<session_id> parent_session;
	// Whether this session carries a fork-inherited event prefix.
	bool is_seeded = false;
	std::uint32_t delegation_depth = 0;
	session_origin origin = session_origin::none;
	std::optional<std::string> agent_preset;
};

// One append-only log entry. `ignorable` marks vocabulary the current
// runtime does not understand: such events are preserved verbatim and
// skipped by the surface, so new event types can be written without a
// format bump (the paper's event-vocabulary growth story).
struct session_event {
	session_seq seq = 0;
	std::int64_t time = 0;
	std::string type;
	boost::json::value data;
	bool ignorable = false;
};

// The four message kinds the surface folds from the log. `tool_result`
// projects onto the LLM-facing role "user".
enum class message_role : std::uint8_t {
	system,
	user,
	assistant,
	tool_result,
};

// One folded surface message. `content` is an array of content blocks (the
// agent loop's schema); `tool_call_id` links a tool result to the assistant
// block that requested it; `source_plugin` names the plugin that authored a
// system message.
struct session_message {
	message_role role = message_role::user;
	std::string id;
	boost::json::value content;
	std::optional<std::string> tool_call_id;
	std::optional<std::string> source_plugin;
};

// Options for creating (or restoring) a session. `seed` replays or forks an
// existing event log; the `meta` fields are folded into the header.
struct create_session_options {
	std::vector<session_event> seed;
	// Exact fork-inherited prefix length when `is_seeded` is true.
	session_log_offset inherited_event_count = 0;
	std::optional<std::string> cwd;
	std::optional<session_id> parent_session;
	std::optional<std::int64_t> created_at;
	bool is_seeded = false;
	session_origin origin = session_origin::none;
	std::optional<std::uint32_t> delegation_depth;
	std::optional<std::string> agent_preset;
};

// The built-in event types the surface understands:
//   user/message     data IS the message:  {id, role:"user", content:[...]}
//   assistant/message data = {"message":   {id, role:"assistant", content:[...]}}
//   system/message   data = {"message":    {id, role:"system", content:[...],
//                                           source:{kind:"plugin", plugin:...}}}
//   tool/result      data IS the message:  {id, role:"user",
//                                           content:[{tool_call_id, ...}],
//                                           source:{kind:"tool", call_id}}
// Everything else is non-surface vocabulary (request metadata, markers,
// plugin-owned types registered as projections, or unknown/ignorable types).
bool is_builtin_surface_type(std::string_view type) noexcept;

inline std::int64_t now_ms() noexcept {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
		.count();
}

} // namespace araya::session
