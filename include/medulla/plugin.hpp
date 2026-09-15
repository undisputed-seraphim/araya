#pragma once

#include "medulla/service.hpp"
#include "medulla/task.hpp"

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace medulla {

class plugin_context;

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

class plugin {
public:
    virtual ~plugin() = default;

    virtual boost::asio::awaitable<void> apply(plugin_context&) = 0;

    virtual bool reconfigure(plugin_config const&) { return false; }
};

struct plugin_descriptor {
    std::string_view name;
    std::span<dependency_spec const> inject;
    std::span<provision_spec const> provide;
    std::function<std::unique_ptr<plugin>(plugin_config const&)> create;
};

class context;

struct component_spec {
    std::shared_ptr<plugin_descriptor> descriptor;
    plugin_config config;
    std::shared_ptr<context> parent;
    std::string name;
    // Per-key isolation realms (Section 5.2.1): each entry maps a service
    // key name to a realm tag. Entries sharing a tag share the binding.
    std::map<std::string, std::string> isolate;
};

}  // namespace medulla
