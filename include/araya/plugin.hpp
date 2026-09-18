#pragma once

#include "araya/service.hpp"
#include "araya/task.hpp"

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace araya {

class plugin_context;

// What a component requires. Declared keys are resolved before apply
// starts; the runtime rejects undeclared access at runtime and the
// metadata here is the component-declared half of Definition 26's merge.
struct dependency_spec {
    service_id key;
    bool required = true;
    // Component-declared interception metadata (Definition 26); merged at
    // access with the context-carried metadata, which takes priority.
    service_metadata metadata;
};

struct provision_spec {
    service_id key;
};

using plugin_config = std::map<std::string, std::string>;

// A component. One instance is created per fiber via the descriptor's
// factory and destroyed when the fiber is retired.
//
// Lifecycle: apply() is the activation - the paper's effect iterator,
// running on the control strand. Each co_await is a step boundary; the
// accumulated cleanups revert the activation if it is cancelled or
// throws. reconfigure() is the optional in-place config update: return
// false to decline, in which case the runtime reloads the fiber (full
// deactivate/reactivate) with the new config.
class plugin {
public:
    virtual ~plugin() = default;

    virtual boost::asio::awaitable<void> apply(plugin_context&) = 0;

    virtual bool reconfigure(plugin_config const&) { return false; }
};

// The static description of a component type.
//
// Lifetime: descriptors are shared and long-lived - the name and the
// inject/provide spans must point at storage that outlives every fiber
// created from the descriptor (static storage in practice). create()
// produces one plugin instance per fiber.
struct plugin_descriptor {
    std::string_view name;
    std::span<dependency_spec const> inject;
    std::span<provision_spec const> provide;
    std::function<std::unique_ptr<plugin>(plugin_config const&)> create;
};

class context;

// One requested component instance: which plugin, with which config, in
// which scope, under which name. The reconcile() diff keys on path, so
// path must be stable across the host's desired-tree updates.
struct component_spec {
    std::shared_ptr<plugin_descriptor> descriptor;
    plugin_config config;
    std::shared_ptr<context> parent;
    std::string name;
    // Per-key isolation realms (Section 5.2.1): each entry maps a service
    // key name to a realm tag. Entries sharing a tag share the binding.
    std::map<std::string, std::string> isolate;
};

}  // namespace araya
