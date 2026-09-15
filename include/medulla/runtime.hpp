#pragma once

#include "medulla/detail/gate.hpp"
#include "medulla/events.hpp"
#include "medulla/fiber_handle.hpp"
#include "medulla/plugin.hpp"
#include "medulla/plugin_context.hpp"
#include "medulla/task.hpp"

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

namespace medulla {

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

    // Registers the sink the declarative-graph diagnostics are reported to.
    // The sink runs on the control strand when a conflict or dependency
    // cycle first appears (each distinct signature is reported once).
    void on_diagnostic(
        std::move_only_function<void(diagnostic const&)> sink);

    // Diagnostics. Checks the engine's internal invariants (lifecycle
    // legality, guard accounting, committed-view hygiene, index
    // consistency, declaration immutability) via MEDULLA_ASSERT: aborts in
    // debug builds, throws std::logic_error when MEDULLA_ENFORCE_INVARIANTS
    // is defined, and is a no-op otherwise. Call it from inside
    // run_on_strand, from a plugin's synchronous teardown, or while the
    // runtime is idle; the engine serializes everything through the
    // single-threaded io_context, which is what makes the reads safe.
    void validate_invariants() const;

    // Posts validate_invariants() to the control strand and waits for it.
    boost::asio::awaitable<void> validate_invariants_async() const;

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

    void notify(service_id key);

    // Removes a destroyed fiber's id from the consumers_of_ index.
    void unindex(fiber_id id);

    // Rebuilds the declared dependency graph and reports newly appeared
    // single-source conflicts and dependency cycles to the diagnostic sink.
    void diagnose();

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
    std::map<std::string, fiber_id> reconciled_;
    std::move_only_function<void(diagnostic const&)> diagnostic_sink_;
    std::set<std::string> reported_diagnostics_;
    std::size_t in_flight_ = 0;
    std::shared_ptr<detail::gate_impl> idle_ = detail::make_gate(true);
};

}  // namespace medulla
