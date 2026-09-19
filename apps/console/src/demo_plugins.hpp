#pragma once

#include "araya/events.hpp"
#include "araya/plugin.hpp"
#include "araya/service.hpp"

#include <atomic>
#include <memory>
#include <string_view>

// The demo components the console app mounts next to the first-party
// plugins: the console itself (a required consumer of logger, timer, and
// sessions), the beacon/watcher availability pair, and the failure-
// injection bombs. All four are ordinary Araya plugins living in the app
// binary - the same composition rules as shipped plugins, so the demo is
// the engine's own semantics, not a special path.
namespace araya::console_demo {

// State shared between main (the host) and the demo components: main
// drives the beacon's availability flag, the console plugin reports its
// activation for the live feed.
struct demo_state {
	std::atomic<bool> beacon_ready{true};
	std::atomic<bool> console_active{false};
};

// Installs the shared state (once, before any descriptor is used) and
// returns it. main owns the shared_ptr; the demo components close over
// it through their factories.
void init_demo_state(std::shared_ptr<demo_state> state);
demo_state& state();

// The console plugin: requires logger, timer, and sessions (so retiring
// any of the three cascades to it), logs its activation, and owns a
// heartbeat interval - the tracked-effect discipline in miniature.
araya::plugin_descriptor const& console_descriptor();

// The availability pair. beacon provides demo.beacon gated on
// demo_state::beacon_ready, evaluated once at provide-time, and promotes
// on the demo.beacon/ready event. watcher requires the beacon (and the
// logger, to announce itself): it parks while the beacon is unavailable
// and activates on promotion.
struct beacon_signal {};

inline constexpr araya::service_key<beacon_signal> beacon_key{"demo.beacon", 1};
inline constexpr araya::event_key<std::string, araya::dispatch_mode::emit> beacon_ready_key{"demo.beacon/ready", 1};

araya::plugin_descriptor const& beacon_descriptor();
araya::plugin_descriptor const& watcher_descriptor();

// The failure-injection bombs: descriptors providing a first-party key
// ("logger", "timer", or "sessions") that throw from apply. They stand
// in for a crashed provider so `fail` can show the recovery path.
araya::plugin_descriptor const& bomb_descriptor_for(std::string_view provided_key);

} // namespace araya::console_demo
