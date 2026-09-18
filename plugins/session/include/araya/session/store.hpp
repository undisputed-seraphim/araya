#pragma once

#include "araya/effects.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/session/events.hpp"
#include "araya/session/session_types.hpp"
#include "araya/session/surface.hpp"
#include "araya/task.hpp"

#include <map>
#include <memory>
#include <string>

namespace araya::session {

class session_store;

// An event-sourced session: an append-only log of session_events plus the
// surface folded from it. Sessions are ordinary objects (not services):
// live instances are created through session_store; detached instances
// exist only between prepare() and enter(). A session holds its store
// weakly, so outliving the store degrades it to a detached shell — appends
// keep working locally but no longer publish.
class session {
public:
	session_id const& id() const noexcept { return header_.id; }

	session_header const& header() const noexcept { return header_; }

	session_log_offset inherited_event_count() const noexcept { return inherited_event_count_; }

	// The first sequence appended in this process: construction input
	// (seed, repair closers, the end-seed marker) never publishes.
	session_seq first_live_seq() const noexcept { return first_live_seq_; }

	session_surface const& surface() const noexcept { return surface_; }

	// The full canonical log, construction seed included (persistence
	// replay starts at seq 0).
	std::vector<session_event> const& log() const noexcept { return log_; }

	// Appends one event: assigns the next sequence, validates the shape of
	// built-in message types, folds the surface, and publishes on the
	// 'session/event' firehose while the session is entered in its store.
	// Returns the assigned sequence.
	session_seq append(std::string type, boost::json::value data);

	// Awaits every 'session/flush' listener (the durability barrier).
	araya::task<void> flush();

private:
	friend class session_store;

	session(
		session_header header,
		session_log_offset inherited_event_count,
		session_seq first_live_seq,
		std::vector<session_event> log,
		std::vector<message_projection> const& projections,
		std::weak_ptr<session_store> store);

	void fold(session_event const& ev, std::vector<message_projection> const& projections);

	session_header header_;
	session_log_offset inherited_event_count_ = 0;
	session_seq first_live_seq_ = 0;
	session_seq next_seq_ = 0;
	std::vector<session_event> log_;
	session_surface surface_;
	std::weak_ptr<session_store> store_;
};

// The store: the one service this plugin provides. It owns every entered
// session, mints ids, validates seeds, folds surfaces through the built-in
// fold plus registered projections, and dispatches the lifecycle events.
//
// Threading: the engine serializes everything through its single-threaded
// control strand, so every method here is synchronous and must be called
// from plugin code (which always runs on that strand).
//
// Fiber ownership: create() attaches an ordinary tracked effect to the
// *calling* fiber, so unloading the caller removes its session — the same
// discipline as any plugin-owned resource. The store itself outlives all
// sessions: destroying it (the sessions provider unloading) disposes every
// entered session, because sessions only make sense while their store does.
class session_store : public std::enable_shared_from_this<session_store> {
public:
	explicit session_store(plugin_context& owner);

	~session_store();

	std::size_t size() const noexcept { return store_.size(); }

	session_id mint_id();

	// Create a session owned by the calling fiber: prepares, enters,
	// attaches the caller-owned cleanup, and announces it. Unloading the
	// caller stops event notification and removes the session.
	std::shared_ptr<session> create(plugin_context& caller, session_id id = {}, create_session_options options = {});

	// Build a session WITHOUT entering it — validates the seed, repairs
	// interrupted turns, stamps the header. Pairs with enter()/announce()
	// for hosts that fold the session lifecycle into one ordered effect
	// chain (the agent-loop pattern).
	std::shared_ptr<session> prepare(session_id id, create_session_options options = {});

	void enter(std::shared_ptr<session> s);

	void announce(session const& s);

	std::shared_ptr<session> get(session_id const& id) const;

	// Removes an entered session and dispatches 'session/disposed'.
	// Idempotent: false when the id was not entered.
	bool dispose(session_id const& id);

	// Registers one event interpreter, owned by the registering fiber
	// (a tracked effect): unloading that fiber removes the projection.
	// A type may have at most one projection.
	araya::registration register_message_projection(plugin_context& caller, message_projection projection);

	// Awaits every 'session/flush' listener for an entered session.
	araya::task<void> flush(session_id id);

private:
	friend class session;

	bool has_projection(std::string_view type) const noexcept;

	// Validates the event envelope and built-in message shapes, marks
	// unknown vocabulary ignorable, folds the surface, and publishes on
	// the firehose (entered sessions only). Throws on shape violations.
	void publish(session& s, session_event& ev);

	void validate_event(session_event const& ev) const;

	std::shared_ptr<araya::event_bus> bus_;
	std::map<session_id, std::shared_ptr<session>> store_;
	std::vector<message_projection> projections_;
	std::uint64_t counter_ = 0;
};

inline constexpr araya::service_key<session_store> sessions_key{"sessions", 1};

// The plugin descriptor: apply() constructs the store and provides it under
// sessions_key. No dependencies; the store dispatches through the owning
// activation's event bus.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::session
