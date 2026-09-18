#pragma once

#include "araya/abi.hpp"
#include "araya/plugin.hpp"

#include <memory>

namespace araya::abi {

// The host-side function table handed to native modules at load time.
// One table per process, valid for the process lifetime.
araya_host_api_v1 const& host_api() noexcept;

// Calls a module entry point immediately and wraps the returned C descriptor
// into a araya plugin_descriptor. The optional keep_alive object is
// retained by the returned descriptor's factory, so a module_loader can tie
// descriptor lifetime to dlopen/delayed dlclose.
std::shared_ptr<plugin_descriptor> wrap(araya_plugin_entry_fn entry, std::shared_ptr<void> keep_alive = nullptr);

} // namespace araya::abi
