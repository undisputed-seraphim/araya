#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"
#include "araya/session/session_types.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The session-persistence seam: a durable backend for session event
// logs. One plugin provides it per profile - the JSONL backend ships
// now, and a different backend can provide the same key later (two
// providers of one key is a declared conflict, which is the engine
// being honest about it).
//
// The plugin (not the backend) subscribes to the session firehose and
// drives attach/append/flush/detach; the backend is pure storage. Both
// run on the control strand: appends buffer in memory, flush is the
// durability point (fsync), and file I/O in listeners is small bounded
// work by design.
namespace araya::persistence {

// One durable snapshot of a stored session: the header as recorded at
// creation plus the full canonical log (seq 0..n-1, contiguous).
struct stored_session {
	session::session_header header;
	std::vector<session::session_event> events;
};

class session_persistence {
public:
	virtual ~session_persistence() = default;

	// Opens (or reopens, truncating any torn tail) the log for a session
	// and starts buffering appends. Runs once per session on
	// 'session/created'.
	virtual void attach(session::session_header const& header) = 0;

	// Buffers one event for the session's live writer; events for
	// sessions without a writer (created before this plugin mounted, or
	// already detached) are dropped - the same coverage rule as the
	// harness's write handles.
	virtual void append(session::session_id const& id, session::session_event const& ev) = 0;

	// The durability point: drains buffered records and fsyncs.
	virtual void flush(session::session_id const& id) = 0;

	// Final flush and release; runs on 'session/disposed'.
	virtual void detach(session::session_id const& id) = 0;

	// Reads a stored session (header + log); nullopt when it was never
	// persisted. Throws on format-version mismatch or a malformed record.
	virtual std::optional<stored_session> read(session::session_id const& id) const = 0;

	// The ids of every session persisted under the root.
	virtual std::vector<session::session_id> list() const = 0;
};

inline constexpr araya::service_key<session_persistence> persistence_key{"session.persistence", 1};

// The JSONL backend factory: the plugin's apply() constructs its backend
// through this, and tests can build one directly. A future backend
// (sqlite) would add its own factory without touching this header.
std::shared_ptr<session_persistence> make_jsonl_backend(std::filesystem::path root);

// The plugin descriptor: apply() constructs the backend from config
// ("root": storage directory) and subscribes to the session firehose.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::persistence
