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
    std::size_t in_flight_ = 0;
    std::shared_ptr<detail::gate_impl> idle_ = detail::make_gate(true);
};

}  // namespace medulla
