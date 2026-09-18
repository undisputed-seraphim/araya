#pragma once

// Araya proof: standalone reference model of the paper's rule system.
//
// This header is deliberately free of any araya/ include: it is an
// independent restatement of the nine rules plus the extensions Araya
// adopts (the Asynchrony/inertia regime, the Failure extension, and
// self-provision exclusion), rewritten from the paper and from the
// engine's documented semantics — never by including engine code.
//
// The model is single-threaded, like the engine: every operation runs to
// a fixpoint (all cascades, notifications, and spawned applies complete)
// before the next one begins, which is exactly what the engine's control
// strand guarantees and what wait_idle() observes.
//
// Observable state (the abstraction contract, see proof/README.md):
//   - per-slot fiber state and error presence,
//   - the binding of the example.db key (which slot provides it),
//   - the lifecycle event order: apply/cleanup markers, in engine order.
//
// Conformance tests drive this machine and the real engine with identical
// operation sequences and require identical observables.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya_proof {

inline constexpr std::string_view db_key = "example.db";

// One component of the test universe. `fails` marks a component whose
// apply throws (the paper's Failure extension: the raise withholds
// re-entry until a revision reinserts the fiber). `reconfigures` marks a
// component whose reconfigure() accepts config changes in place.
struct component {
	std::string name;
	std::vector<std::pair<std::string, bool>> inject; // (key, required)
	std::vector<std::string> provide;
	bool fails = false;
	bool reconfigures = false;
};

enum class fstate : std::uint8_t { inactive, loading, active, unloading };
enum class pstate : std::uint8_t { loading, active, retiring };

struct binding {
	int provider = -1; // slot index, -1 when unbound
	pstate state = pstate::active;
};

// The reference machine. Slots are component instances (the orchestrator's
// insertion slots); components are identified by the pool character.
class machine {
public:
	explicit machine(std::map<char, component> pool)
		: pool_(std::move(pool)) {}

	// -- operations (each runs the machine to its fixpoint) --------------
	// Inserts a component into an empty slot (imperative mount; the fiber
	// is not part of the declared state).
	void mount(int slot, char comp, std::string config = "1");
	// Retires an imperatively mounted slot: unloads its fiber (cascading
	// to consumers) and erases the record (O-Retire, then O-Remove).
	void retire(int slot);
	// Declarative reconcile over the FULL desired state: every reconciled
	// slot absent from `desired` is retired and erased; every desired
	// slot is mounted (if absent), kept (same component + same config),
	// reconfigure()d in place (config change, component accepts), or
	// rebuilt (anything else).
	void reconcile_all(std::map<int, std::pair<char, std::string>> const& desired);

	// -- observables (refreshed by every operation) ----------------------
	std::map<int, fstate> slot_states;
	std::map<int, bool> slot_errors;
	char db_provider = '\0';	  // component bound to example.db
	std::vector<std::string> log; // "apply:<name>", "cleanup:<name>"

private:
	struct fiber {
		int fidx = -1; // mount-order index (mirrors the engine's fiber ids)
		int slot = -1;
		char comp = 0;
		bool alive = true;
		fstate state = fstate::inactive;
		bool apply_failed = false;
		bool has_error = false;
		int reactivate = 0;
		std::map<std::string, int> committed; // key -> provider slot
		std::set<int> consumers;			  // slots committed to us
		int remaining = 0;
		std::string config;
	};

	component const& comp(fiber const& f) const { return pool_.at(f.comp); }

	fiber* at(int fidx) {
		auto it = fibers_.find(fidx);
		return it == fibers_.end() ? nullptr : &it->second;
	}

	fiber* at_slot(int slot) {
		auto it = slot_of_.find(slot);
		if (it == slot_of_.end() || it->second < 0)
			return nullptr;
		return at(it->second);
	}

	binding const* lookup(std::string_view key) const {
		auto it = bindings_.find(std::string(key));
		return it == bindings_.end() ? nullptr : &it->second;
	}

	// Resolution (the paper's table lookup under the last-bind-wins
	// binding map): a key resolves to an active binding that is not the
	// fiber's own (self-provision exclusion).
	bool satisfiable(fiber const& f, std::map<std::string, int>& providers) const {
		for (auto const& [key, required] : comp(f).inject) {
			auto const* b = lookup(key);
			if (b && b->provider != -1 && b->state == pstate::active && b->provider != f.fidx) {
				providers[key] = b->provider;
			} else if (required) {
				return false;
			}
		}
		return true;
	}

	bool providers_still_active(fiber const& f) const {
		for (auto const& [key, p] : f.committed) {
			auto const* b = lookup(key);
			if (!b || b->provider == -1 || b->provider != p || b->state != pstate::active)
				return false;
		}
		return true;
	}

	void evaluate(fiber& f);
	void start_loading(fiber& f, std::map<std::string, int> providers);
	void apply_completed(fiber& f, bool failure);
	void publish(fiber& f);
	void begin_unload(fiber& f, bool flag);
	void maybe_finish(fiber& f);
	void finish(fiber& f);
	void teardown(fiber& f);
	void notify(std::string_view key);
	void erase(fiber& f);
	void drain();
	void refresh();

	std::map<char, component> pool_;
	std::map<int, fiber> fibers_; // fidx -> fiber
	std::map<int, int> slot_of_;  // slot -> fidx (-1 when empty)
	std::set<int> reconciled_;	  // slots under declared-state control
	int next_fidx_ = 1;
	std::map<std::string, binding> bindings_;
	// Consumer registry per declared key, in mount order (the engine's
	// consumers_of_ index, iterated by fiber id).
	std::map<std::string, std::vector<int>> consumers_of_;
	std::deque<int> work_; // pending applies, in spawn order
};

inline void machine::mount(int slot, char comp, std::string config) {
	if (slot_of_.contains(slot) && slot_of_[slot] >= 0)
		return;
	fiber f;
	f.fidx = next_fidx_++;
	f.slot = slot;
	f.comp = comp;
	f.config = std::move(config);
	for (auto const& [key, required] : pool_.at(comp).inject)
		consumers_of_[key].push_back(f.fidx);
	slot_of_[slot] = f.fidx;
	fibers_.emplace(f.fidx, std::move(f));
	evaluate(*at(f.fidx));
	drain();
	refresh();
}

inline void machine::retire(int slot) {
	auto* f = at_slot(slot);
	if (!f)
		return;
	f->reactivate = -1;
	begin_unload(*f, false);
	drain();
	erase(*f);
	refresh();
}

inline void machine::reconcile_all(std::map<int, std::pair<char, std::string>> const& desired) {
	// Retire every reconciled slot the desired state no longer declares.
	for (auto it = reconciled_.begin(); it != reconciled_.end();) {
		int slot = *it;
		auto dit = desired.find(slot);
		if (dit != desired.end()) {
			++it;
			continue;
		}
		if (auto* f = at_slot(slot); f) {
			f->reactivate = -1;
			begin_unload(*f, false);
			drain();
			erase(*f);
		}
		it = reconciled_.erase(it);
	}
	for (auto const& [slot, wanted] : desired) {
		auto const& [comp, config] = wanted;
		auto* f = at_slot(slot);
		bool declared = reconciled_.contains(slot);
		if (f && declared) {
			bool same_impl = pool_.at(comp).name == pool_.at(f->comp).name;
			bool same_cfg = f->config == config;
			bool keep = same_impl && !same_cfg && f->state == fstate::active && pool_.at(comp).reconfigures;
			if (same_impl && same_cfg) {
				continue;
			}
			if (keep) {
				f->config = config;
				continue;
			}
			f->reactivate = -1;
			begin_unload(*f, false);
			drain();
			erase(*f);
			reconciled_.erase(slot);
			f = nullptr;
		}
		if (!f) {
			mount(slot, comp, config);
			reconciled_.insert(slot);
		}
	}
	drain();
	refresh();
}

inline void machine::evaluate(fiber& f) {
	if (f.state != fstate::inactive && f.state != fstate::active)
		return;
	if (f.reactivate < 0)
		return;
	std::map<std::string, int> providers;
	bool sat = satisfiable(f, providers);
	if (f.state == fstate::inactive) {
		if (sat && !f.apply_failed) {
			start_loading(f, std::move(providers));
		} else if (!f.has_error) {
			f.has_error = true; // resolution failure, recorded once
		}
		return;
	}
	if (sat && providers == f.committed)
		return;
	begin_unload(f, sat);
}

inline void machine::start_loading(fiber& f, std::map<std::string, int> providers) {
	f.state = fstate::loading;
	f.has_error = false;
	f.apply_failed = false;
	f.committed = std::move(providers);
	work_.push_back(f.fidx);
}

inline void machine::apply_completed(fiber& f, bool failure) {
	bool stale = !providers_still_active(f);
	bool cancelled = f.state == fstate::unloading;
	if (failure || cancelled || stale || f.state == fstate::unloading) {
		if (failure) {
			f.has_error = true;
			f.apply_failed = true; // withholds re-entry until revision
		} else if (cancelled && !f.has_error) {
			f.has_error = true;
		}
		teardown(f);
		f.state = fstate::inactive;
		f.committed.clear();
		if (stale && f.reactivate >= 0)
			evaluate(f);
		return;
	}
	publish(f);
}

inline void machine::publish(fiber& f) {
	for (auto const& key : comp(f).provide)
		bindings_[key] = binding{f.fidx, pstate::active};
	f.state = fstate::active;
	for (auto const& [key, p] : f.committed)
		if (auto* provider = at(p); provider)
			provider->consumers.insert(f.fidx);
	for (auto const& key : comp(f).provide)
		notify(key);
}

inline void machine::begin_unload(fiber& f, bool flag) {
	if (f.reactivate >= 0)
		f.reactivate = std::max(f.reactivate, flag ? 1 : 0);
	if (f.state == fstate::unloading || f.state == fstate::inactive)
		return;
	if (f.state == fstate::loading) {
		// The pending apply completes cancelled.
		f.state = fstate::unloading;
		return;
	}
	f.state = fstate::unloading;
	for (auto const& key : comp(f).provide) {
		auto it = bindings_.find(key);
		if (it != bindings_.end() && it->second.provider == f.fidx)
			it->second.state = pstate::retiring;
	}
	// The cascade runs over a copy: consumer finishes erase from
	// f.consumers while we are still cascading (the same invalidation
	// the engine guards against by iterating a moved-out copy).
	auto consumers = f.consumers;
	for (auto c : consumers)
		if (auto* consumer = at(c); consumer && consumer->state != fstate::inactive)
			++f.remaining;
	for (auto c : consumers)
		if (auto* consumer = at(c); consumer)
			begin_unload(*consumer, true);
	maybe_finish(f);
}

inline void machine::maybe_finish(fiber& f) {
	if (f.state == fstate::unloading && f.remaining == 0)
		finish(f);
}

inline void machine::finish(fiber& f) {
	teardown(f);
	auto committed = std::move(f.committed);
	f.committed.clear();
	f.state = fstate::inactive;
	for (auto const& [key, p] : committed) {
		if (auto* provider = at(p); provider) {
			provider->consumers.erase(f.fidx);
			if (provider->remaining > 0) {
				--provider->remaining;
				maybe_finish(*provider);
			}
		}
	}
	for (auto const& key : comp(f).provide)
		notify(key);
	if (f.reactivate > 0) {
		f.reactivate = 0;
		evaluate(f);
	}
}

inline void machine::teardown(fiber& f) {
	for (auto const& key : comp(f).provide) {
		auto it = bindings_.find(key);
		if (it != bindings_.end() && it->second.provider == f.fidx)
			bindings_.erase(it);
	}
	if (!comp(f).fails)
		log.push_back("cleanup:" + comp(f).name);
}

inline void machine::notify(std::string_view key) {
	auto it = consumers_of_.find(std::string(key));
	if (it == consumers_of_.end())
		return;
	for (int fidx : it->second)
		if (auto* consumer = at(fidx); consumer)
			evaluate(*consumer);
}

inline void machine::erase(fiber& f) {
	f.alive = false;
	for (auto& [key, slots] : consumers_of_)
		std::erase(slots, f.fidx);
	if (slot_of_[f.slot] == f.fidx)
		slot_of_[f.slot] = -1;
	fibers_.erase(f.fidx);
}

inline void machine::drain() {
	while (!work_.empty()) {
		int fidx = work_.front();
		work_.pop_front();
		auto* f = at(fidx);
		if (!f)
			continue;
		log.push_back("apply:" + comp(*f).name);
		apply_completed(*f, comp(*f).fails);
	}
}

inline void machine::refresh() {
	slot_states.clear();
	slot_errors.clear();
	for (int slot = 0; slot < 2; ++slot) {
		auto* f = at_slot(slot);
		if (!f) {
			slot_states[slot] = fstate::inactive;
			slot_errors[slot] = false;
		} else {
			slot_states[slot] = f->state;
			slot_errors[slot] = f->has_error;
		}
	}
	auto it = bindings_.find(std::string(db_key));
	if (it == bindings_.end() || it->second.provider < 0) {
		db_provider = '\0';
		return;
	}
	auto* f = at(it->second.provider);
	db_provider = f ? f->comp : '\0';
}

} // namespace araya_proof
