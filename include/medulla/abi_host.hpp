#pragma once

#include "medulla/abi.hpp"
#include "medulla/plugin.hpp"

#include <memory>

namespace medulla::abi {

// The host-side function table handed to native modules at load time.
medulla_host_api_v1 const& host_api() noexcept;

// Calls a module entry point immediately and wraps the returned C descriptor
// into a medulla plugin_descriptor. The optional keep_alive object is
// retained by the returned descriptor's factory, so a module_loader can tie
// descriptor lifetime to dlopen/delayed dlclose.
std::shared_ptr<plugin_descriptor> wrap(
    medulla_plugin_entry_fn entry,
    std::shared_ptr<void> keep_alive = nullptr);

}  // namespace medulla::abi
