#pragma once

#include "araya/detail/gate.hpp"
#include "araya/events.hpp"
#include "araya/fiber_handle.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/task.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <cstddef>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace araya {

struct desired_component {
    std::string path;
    component_spec spec;
};

// A declarative-dependency diagnosis, emitted per the paper's Section 6.5:
// cycles and provider conflicts are predictable from the declarations alone,
// so the runtime can report them when components are loaded.
enum class diagnostic_kind {
    conflict,  // two or more fibers provide one key in one scope
    cycle,     // a dependency cycle, including a fiber that declares a key it
               // provides itself
};

struct diagnostic {
    diagnostic_kind kind = diagnostic_kind::conflict;
    std::string key_name;
    std::uint32_t key_version = 1;
    std::vector<fiber_id> fibers;
};

// A read-only snapshot of one mounted fiber, produced by runtime::fibers()
// and fibers_async(). Diagnostics-grade: no API-stability promise, and
// safe to read while the runtime is idle or on the control strand (the
// same contract as state_of()).
struct fiber_info {
    fiber_id id = 0;
    // The component_spec's instance name (may be empty).
    std::string name;
    // The plugin descriptor's name.
    std::string descriptor;
    fiber_state state = fiber_state::inactive;
    std::exception_ptr error;
    // The instantiating fiber, or 0 for an orchestrator-level insertion.
    std::uint64_t parent = 0;
    std::shared_ptr<context> scope;
    // Declared dependency and provision keys (immutable, Lemma 59(5)).
    std::vector<owned_service_id> inject;
    std::vector<owned_service_id> provide;
    // The committed view: declared key -> providing fiber id.
    std::map<owned_service_id, std::uint64_t, transparent_id_less>
        committed;
};

class runtime {
public:
    explicit runtime(boost::asio::any_io_executor ex);

    ~runtime();

    plugin_context root_context();

    boost::asio::awaitable<fiber_handle> mount(component_spec spec);

    boost::asio::awaitable<void> retire(fiber_handle h);

    boost::asio::awaitable<void> wait_idle();

    boost::asio::awaitable<void> reconcile(
        std::vector<desired_component> desired);

    // Resumes on the control strand and invokes fn there.
    boost::asio::awaitable<void> run_on_strand(
        std::move_only_function<void()> fn);

    std::shared_ptr<event_bus> bus() const noexcept { return bus_; }

    std::shared_ptr<context> root() const noexcept { return root_context_; }

    std::size_t fiber_count() const noexcept;

    fiber_state state_of(fiber_id id) const noexcept;

    std::exception_ptr error_of(fiber_id id) const noexcept;

    // Read-only snapshot of every mounted fiber. Call on the control
    // strand or while the runtime is idle.
    std::vector<fiber_info> fibers() const;

    // Posts the snapshot to the control strand and returns it; safe to
    // call from any coroutine.
    boost::asio::awaitable<std::vector<fiber_info>> fibers_async() const;

    // Registers the sink the declarative-graph diagnostics are reported to.
    // The sink runs on the control strand when a conflict or dependency
    // cycle first appears (each distinct signature is reported once).
    void on_diagnostic(
        std::move_only_function<void(diagnostic const&)> sink);

    // Mounts a component synchronously; must be called on the control
    // strand. parent names the instantiating fiber, or 0 for an
    // orchestrator-level insertion.
    fiber_handle mount_locked(component_spec spec, fiber_id parent = 0);

    // Requests retirement of an instantiated child: marks it retired,
    // unloads it, and drops the record once it is inactive (the O-Retire
    // inverse of Definition 52). Safe to call multiple times and after a
    // host-side retire.
    void retire_child(fiber_id id);

    // Diagnostics. Checks the engine's internal invariants (lifecycle
    // legality, guard accounting, committed-view hygiene, index
    // consistency, declaration immutability) via ARAYA_ASSERT: aborts in
    // debug builds, throws std::logic_error when ARAYA_ENFORCE_INVARIANTS
    // is defined, and is a no-op otherwise. Call it from inside
    // run_on_strand, from a plugin's synchronous teardown, or while the
    // runtime is idle; the engine serializes everything through the
    // single-threaded io_context, which is what makes the reads safe.
    void validate_invariants() const;

    // Posts validate_invariants() to the control strand and waits for it.
    boost::asio::awaitable<void> validate_invariants_async() const;

    // Promotes the provision of key (held by the provider fiber in scope)
    // to available and re-evaluates the dependents: ArayaMachine.tla's
    // AvailabilityFlip, which Refine.tla reads as the paper's L-Finish
    // through the lagged provider state. Promotion is idempotent.
    // Deactivation is retirement: marking a provision unavailable throws
    // (the paper's lifecycle DAG has no active -> loading edge). Runs on
    // the control strand.
    void signal_availability(service_id key, context* scope,
                             std::uint64_t provider, bool available);

private:
    struct fiber_record;
    struct resolution;

    resolution resolve(fiber_record const& f) const;

    void evaluate(fiber_record& f);

    void start_loading(fiber_record& f);

    void on_apply_completed(fiber_id id, std::exception_ptr ep);

    bool providers_still_active(fiber_record const& f) const;

    void publish(fiber_record& f);

    void begin_unload(fiber_record& f, bool reactivate);

    void maybe_finish_unload(fiber_record& f);

    void finish_unload(fiber_record& f);

    // Re-evaluates fibers whose declarations include key, provided the
    // changed binding's scope is visible to them and their realm tag for
    // the key matches the binding's realm (Algorithm 3).
    void notify(service_id key, context const* scope,
                std::string const& realm);

    // Moves a fiber's own bindings to a new scope/realm without retiring
    // the provider (the Section 5.2.1 realm reassignment shortcut).
    // old_realms records the realm each provided key was bound under before
    // the entry's tags were updated.
    void reassign(
        fiber_record& f, component_spec const& next,
        std::shared_ptr<context> const& new_scope,
        std::map<owned_service_id, std::string, transparent_id_less> const&
            old_realms);

    // Returns (and maintains) the per-entry realm-tag context for an entry
    // with an isolate annotation, or the entry's plain parent scope.
    std::shared_ptr<context> entry_scope_for(
        std::string const& path, component_spec const& spec);

    // Removes a destroyed fiber's id from the consumers_of_ index.
    void unindex(fiber_id id);

    // Rebuilds the declared dependency graph and reports newly appeared
    // single-source conflicts and dependency cycles to the diagnostic sink.
    // scan_cycles=false skips the Tarjan pass: safe when the event that
    // triggered the scan cannot have created a new cycle signature (see
    // mount_locked's gate), i.e. the new fiber declares no key that any
    // provider — itself included — provides. The conflict scan and the
    // self-provision pass always run.
    void diagnose(bool scan_cycles = true);

    void transition_started();
    void transition_finished();
    bool settled() const noexcept { return in_flight_ == 0; }

    fiber_record* find(fiber_id id) noexcept;

    boost::asio::strand<boost::asio::any_io_executor> strand_;
    std::shared_ptr<event_bus> bus_;
    std::shared_ptr<context> root_context_;
    std::shared_ptr<activation> root_activation_;
    std::map<fiber_id, std::unique_ptr<fiber_record>> fibers_;
    std::map<owned_service_id, std::set<fiber_id>, transparent_id_less>
        consumers_of_;
    // The provider mirror of consumers_of_: which fibers provide each
    // key. Maintained exactly like consumers_of_ (insert at mount, erase
    // in unindex; provide keys are immutable, so reassign needs nothing).
    std::map<owned_service_id, std::set<fiber_id>, transparent_id_less>
        providers_of_;
    std::map<std::string, fiber_id> reconciled_;
    std::map<std::string, std::shared_ptr<context>> entry_ctxs_;
    std::map<std::string, std::map<std::string, std::string>>
        entry_isolates_;
    std::move_only_function<void(diagnostic const&)> diagnostic_sink_;
    std::set<std::string> reported_diagnostics_;

    // A provider slot for the single-source scan: one (scope, realm, key)
    // group. Ordered by (scope, realm, name, version) — equivalent to the
    // old "(realm@)name:version" string key except for pathological
    // realm/name strings containing '@' or ':' (which the string form
    // accidentally merged).
    struct diag_slot_key {
        context const* scope = nullptr;
        owned_service_id key;
        std::string realm;

        friend bool operator<(diag_slot_key const& a,
                              diag_slot_key const& b) noexcept {
            if (std::less<context const*>{}(a.scope, b.scope))
                return true;
            if (std::less<context const*>{}(b.scope, a.scope))
                return false;
            if (auto c = a.realm.compare(b.realm); c != 0)
                return c < 0;
            if (auto c = a.key.name.compare(b.key.name); c != 0)
                return c < 0;
            return a.key.version < b.key.version;
        }
    };

    // diagnose() scratch space, reused across calls (clear()/assign()
    // only, so a rescan allocates nothing after the first call).
    std::vector<std::pair<diag_slot_key, fiber_id>> diag_slots_;
    std::vector<fiber_id> diag_order_;
    std::vector<std::vector<fiber_id>> diag_adj_;
    std::vector<fiber_id> diag_targets_;
    std::vector<int> diag_index_;
    std::vector<int> diag_low_;
    std::vector<int> diag_stack_;
    std::vector<unsigned char> diag_on_stack_;
    std::vector<fiber_id> diag_scc_;

    std::size_t in_flight_ = 0;
    std::shared_ptr<detail::gate_impl> idle_ = detail::make_gate(true);
};

}  // namespace araya
