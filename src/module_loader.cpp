#include "medulla/module_loader.hpp"

#include "medulla/abi.hpp"
#include "medulla/abi_host.hpp"

#include <dlfcn.h>

#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace medulla {

struct module_loader::module_impl {
    explicit module_impl(void* h) : handle(h) {}
    ~module_impl() {
        if (handle)
            dlclose(handle);
    }

    module_impl(module_impl const&) = delete;
    module_impl& operator=(module_impl const&) = delete;

    void* handle;
};

struct module_loader::state {
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<module_impl>> modules;
};

module_loader::module_loader() : state_(std::make_shared<state>()) {}

module_loader::~module_loader() = default;

std::shared_ptr<plugin_descriptor> module_loader::load(
    std::string const& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* error = dlerror();
        throw std::runtime_error("dlopen failed for '" + path +
                                 "': " + (error ? error : "unknown error"));
    }

    auto module = std::make_shared<module_impl>(handle);

    auto entry = reinterpret_cast<medulla_plugin_entry_fn>(
        dlsym(handle, "medulla_plugin_entry_v1"));
    if (!entry)
        throw std::runtime_error("module '" + path +
                                 "' does not export medulla_plugin_entry_v1");

    try {
        auto descriptor = abi::wrap(entry, module);
        {
            std::lock_guard lock{state_->mutex};
            state_->modules[path] = module;
        }
        return descriptor;
    } catch (...) {
        throw;
    }
}

std::size_t module_loader::module_count() const noexcept {
    std::lock_guard lock{state_->mutex};
    std::size_t count = 0;
    for (auto it = state_->modules.begin(); it != state_->modules.end();) {
        if (it->second.expired())
            it = state_->modules.erase(it);
        else {
            ++count;
            ++it;
        }
    }
    return count;
}

}  // namespace medulla
