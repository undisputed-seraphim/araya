#pragma once

#include "araya/effects.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/task.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <functional>
#include <memory>

namespace araya::timer {

// Disposal-aware timers: every timeout/interval is a tracked effect on the
// registering fiber, so unloading the fiber cancels its timers. Callbacks
// run on the engine's control strand (the executor the service was
// constructed with); sleep is a plain awaitable delay.
class timer_service {
public:
    using duration = std::chrono::steady_clock::duration;

    explicit timer_service(boost::asio::any_io_executor executor);

    // Runs fn once after d, unless the owning fiber unloads first.
    araya::registration timeout(plugin_context& caller, duration d,
                                  std::move_only_function<void()> fn);

    // Runs fn every d; returning false (or the owning fiber unloading)
    // stops the interval.
    araya::registration interval(plugin_context& caller, duration d,
                                   std::move_only_function<bool()> fn);

    // Awaits a plain delay.
    araya::task<void> sleep(duration d);

private:
    boost::asio::any_io_executor executor_;
};

inline constexpr araya::service_key<timer_service> timer_key{"timer", 1};

// The plugin descriptor: apply() constructs the service on the owning
// activation's bus executor and provides it under timer_key.
araya::plugin_descriptor const& plugin_descriptor();

}  // namespace araya::timer
