#pragma once

#include "araya/plugin.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace araya {

// Loads native plugins (shared objects exporting araya_plugin_entry_v1).
//
// RESIDENCY: the loader defers each dlclose until the last descriptor,
// plugin instance, and listener callback produced from the module are
// gone - fibers created from the descriptor keep the module resident
// until fully retired, so no code or vtable outlives its mapping. The
// loader itself must outlive every descriptor it produced.
class module_loader {
public:
    module_loader();
    ~module_loader();

    module_loader(module_loader const&) = delete;
    module_loader& operator=(module_loader const&) = delete;

    // Loads a native module and returns a plugin descriptor backed by it.
    // The module is dlclose'd only when the last descriptor, plugin instance,
    // and listener callback produced from it are gone: fibers created from
    // the descriptor keep the module resident until they are fully retired.
    std::shared_ptr<plugin_descriptor> load(std::string const& path);

    std::size_t module_count() const noexcept;

private:
    struct module_impl;
    struct state;
    std::shared_ptr<state> state_;
};

}  // namespace araya
