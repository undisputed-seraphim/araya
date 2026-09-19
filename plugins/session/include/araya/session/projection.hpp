#pragma once

#include "araya/session/session_types.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace araya::session {

class session_store;

// A typed, store-driven state fold over one session's event log - the
// harness's session-projection idea, engine-native: no central registry.
// The store owns the cells; the registering plugin owns the State type
// and reads through the tracker that register_projection fills in.
//
// The store keeps one State cell per entered session: announce() seeds
// it with init(header) and replays the session's log through apply(),
// publish() drives it with every appended event whose type is listed in
// `types` (all types when empty), and dispose() drops the cell. Sessions
// entered before the projection registered have no cell - the same
// coverage rule as the persistence plugin's write handles. Restoring a
// session re-seeds the cell from the replayed log, so projection state
// needs no separate checkpointing.
template <class State>
struct event_projection {
	std::string name;
	// Event-type filter: apply() runs only for these types; empty =
	// every event. Built-in and projection-derived state all flow
	// through the same seq-ordered drive.
	std::vector<std::string> types;
	// Seeds a fresh cell from the session header.
	std::function<State(session_header const&)> init;
	// Drives the cell with one event, in seq order.
	std::function<void(State&, session_event const&)> apply;
};

// The read handle for one registered projection: state_of() returns the
// fold state of an entered session, or null when the session is gone,
// the projection was unregistered, or the session predates it. Must be
// called on the control strand, like every other store method.
template <class State>
class projection_state {
public:
	projection_state() = default;

	State const* state_of(session_id const& id) const;

private:
	friend class session_store;

	std::weak_ptr<session_store> store_;
	std::uint64_t id_ = 0;
};

namespace detail {

// The type-erased cell the store drives; the typed cell is the only
// instantiation.
struct projection_cell_base {
	virtual ~projection_cell_base() = default;
	virtual void seed(session_header const& header, std::vector<session_event> const& log) = 0;
	virtual void drive(session_id const& id, session_event const& ev) = 0;
	virtual void drop(session_id const& id) = 0;
};

template <class State>
struct projection_cell : projection_cell_base {
	explicit projection_cell(event_projection<State> spec)
		: spec_(std::move(spec)) {}

	void seed(session_header const& header, std::vector<session_event> const& log) override {
		auto [it, inserted] = cells_.emplace(header.id, spec_.init(header));
		for (auto const& ev : log) {
			if (accepts(ev.type))
				spec_.apply(it->second, ev);
		}
	}

	void drive(session_id const& id, session_event const& ev) override {
		if (!accepts(ev.type))
			return;
		auto it = cells_.find(id);
		if (it == cells_.end())
			return;
		spec_.apply(it->second, ev);
	}

	void drop(session_id const& id) override { cells_.erase(id); }

	State const* state_of(session_id const& id) const {
		auto it = cells_.find(id);
		return it == cells_.end() ? nullptr : &it->second;
	}

private:
	bool accepts(std::string_view type) const {
		if (spec_.types.empty())
			return true;
		return std::find(spec_.types.begin(), spec_.types.end(), std::string(type)) != spec_.types.end();
	}

	event_projection<State> spec_;
	std::map<session_id, State> cells_;
};

} // namespace detail

} // namespace araya::session
