#pragma once

#include "medulla/context.hpp"
#include "medulla/effects.hpp"
#include "medulla/fiber_handle.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <stop_token>
#include <vector>

namespace medulla {

class event_bus;
class runtime;

class activation : public std::enable_shared_from_this<activation> {
public:
    explicit activation(std::shared_ptr<context> scope);
    ~activation();

    std::uint64_t id = 0;
    std::shared_ptr<context> scope;
    std::shared_ptr<effect_stack> effects;
    std::shared_ptr<std::stop_source> stop_source;
    std::shared_ptr<event_bus> bus;
    std::exception_ptr error;

    // The activation of the fiber that instantiated this one, if any; the
    // chain realizes the parent-fiber walk of Algorithm 6.
    std::shared_ptr<activation> parent;
    // The runtime this activation belongs to; enables plugin_context::mount.
    runtime* owner = nullptr;

    fiber_state state = fiber_state::active;
    bool spec_declared = false;
    std::vector<owned_service_id> inject_specs;
    std::vector<owned_service_id> provide_specs;
    // Component-declared interception metadata per injected key.
    std::map<owned_service_id, service_metadata, transparent_id_less>
        inject_metadata;
    std::map<owned_service_id, binding, transparent_id_less> committed_view;

    std::stop_token stop_token() const noexcept;

    void teardown() noexcept;
};

}  // namespace medulla
