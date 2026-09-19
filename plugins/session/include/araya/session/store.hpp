#pragma once

#include "araya/effects.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/session/events.hpp"
#include "araya/session/projection.hpp"
#include "araya/session/session_types.hpp"
#include "araya/session/surface.hpp"
#include "araya/task.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

	// The ids of every entered session, in insertion order.
	std::vector<session_id> list() const;

	session_id mint_id();

	// Create a session owned by the calling fiber: prepares, enters,
	// attaches the caller-owned cleanup, and announces it. Unloading the
	// caller stops event notification and removes the session.
	std::shared_ptr<session> create(plugin_context& caller, session_id id = {}, create_session_options options = {});

	// Forks a session: the child opens with the parent's log prefix up to
	// `cut` (the whole log by default) as its inherited seed, carries the
	// lineage (parent_session, is_seeded, and the 'session/end-seed'
	// marker when the cut is non-empty), and continues with fresh history
	// after it. cwd, origin, delegation depth, and preset inherit from the
	// parent; an empty child_id mints a fresh one. The child is entered
	// and announced, owned by the calling fiber like any create().
	std::shared_ptr<session> fork(
		plugin_context& caller,
		session_id const& parent,
		session_id child_id = {},
		std::optional<session_log_offset> cut = std::nullopt);

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

	// Registers a typed, store-driven state fold (the projection
	// companion): the store owns one State cell per entered session and
	// drives it in seq order - see araya/session/projection.hpp. Owned by
	// the registering fiber (a tracked effect); the out-parameter tracker
	// is the read handle.
	template <class State>
	araya::registration
	register_projection(plugin_context& caller, event_projection<State> projection, projection_state<State>& tracker);

private:
	friend class session;

	template <class State>
	friend class projection_state;

	bool has_projection(std::string_view type) const noexcept;

	// Validates the event envelope and built-in message shapes, marks
	// unknown vocabulary ignorable, folds the surface, and publishes on
	// the firehose (entered sessions only). Throws on shape violations.
	void publish(session& s, session_event& ev);

	void validate_event(session_event const& ev) const;

	// The projection read path (projection_state::state_of): the cell's
	// typed state for an entered session, or null.
	template <class State>
	State const* projection_state_of(std::uint64_t projection_id, session_id const& sid) const;

	std::shared_ptr<araya::event_bus> bus_;
	std::map<session_id, std::shared_ptr<session>> store_;
	std::vector<message_projection> projections_;
	std::map<std::uint64_t, std::unique_ptr<detail::projection_cell_base>> projection_cells_;
	std::uint64_t projection_counter_ = 0;
	std::uint64_t counter_ = 0;
};

// register_projection's template body lives here (store.hpp is the
// projection surface users include); the cells and trackers are declared
// in projection.hpp.
template <class State>
araya::registration session_store::register_projection(
	plugin_context& caller,
	event_projection<State> projection,
	projection_state<State>& tracker) {
	auto id = projection_counter_++;
	projection_cells_.emplace(id, std::make_unique<detail::projection_cell<State>>(std::move(projection)));
	tracker.store_ = weak_from_this();
	tracker.id_ = id;
	std::weak_ptr<session_store> weak = weak_from_this();
	return caller.effect([weak, id]() -> araya::cleanup_action {
		return [weak, id] {
			if (auto st = weak.lock())
				st->projection_cells_.erase(id);
		};
	});
}

template <class State>
State const* session_store::projection_state_of(std::uint64_t projection_id, session_id const& sid) const {
	auto it = projection_cells_.find(projection_id);
	if (it == projection_cells_.end())
		return nullptr;
	return static_cast<detail::projection_cell<State>*>(it->second.get())->state_of(sid);
}

template <class State>
State const* projection_state<State>::state_of(session_id const& id) const {
	auto st = store_.lock();
	if (!st)
		return nullptr;
	return st->projection_state_of<State>(id_, id);
}

inline constexpr araya::service_key<session_store> sessions_key{"sessions", 1};

// The plugin descriptor: apply() constructs the store and provides it under
// sessions_key. No dependencies; the store dispatches through the owning
// activation's event bus.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::session
