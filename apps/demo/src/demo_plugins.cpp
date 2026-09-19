#include "demo_plugins.hpp"

#include "araya/logger/logger.hpp"
#include "araya/plugin_context.hpp"
#include "araya/session/store.hpp"
#include "araya/task.hpp"
#include "araya/timer/timer.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace araya::console_demo {
namespace {

using namespace std::chrono_literals;

std::shared_ptr<demo_state> g_state;

// -- console -----------------------------------------------------------

struct console_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto logger = ctx.require<araya::logger::logger_service>(araya::logger::logger_key).shared();
		auto timer = ctx.require<araya::timer::timer_service>(araya::timer::timer_key).shared();
		(void)ctx.require<araya::session::session_store>(araya::session::sessions_key);

		auto log = logger->named("console");
		ctx.effect([&]() -> araya::cleanup_action {
			g_state->console_active.store(true, std::memory_order_release);
			log.info("console: online");
			return [log] {
				g_state->console_active.store(false, std::memory_order_release);
				log.info("console: offline");
			};
		});
		// The heartbeat is a tracked effect on this fiber: unloading the
		// console cancels it, and the demo watches it die with the fiber.
		(void)timer->interval(ctx, 8s, [log] {
			log.debug("console: heartbeat");
			return true;
		});
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_console(araya::plugin_config const&) { return std::make_unique<console_plugin>(); }

// -- beacon / watcher ---------------------------------------------------

struct beacon_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		ctx.provide(beacon_key, std::make_shared<beacon_signal>(), [] {
			return g_state->beacon_ready.load(std::memory_order_acquire);
		});
		ctx.on(beacon_ready_key, [ctx](std::string const&) {
			g_state->beacon_ready.store(true, std::memory_order_release);
			ctx.set_available(beacon_key, true);
		});
		co_return;
	}
};

struct watcher_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		(void)ctx.require<beacon_signal>(beacon_key);
		auto logger = ctx.require<araya::logger::logger_service>(araya::logger::logger_key).shared();
		logger->named("watcher").info("watcher: beacon acquired");
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_beacon(araya::plugin_config const&) { return std::make_unique<beacon_plugin>(); }
std::unique_ptr<araya::plugin> make_watcher(araya::plugin_config const&) { return std::make_unique<watcher_plugin>(); }

// -- bombs --------------------------------------------------------------

struct bomb_plugin : araya::plugin {
	explicit bomb_plugin(std::string what)
		: what_(std::move(what)) {}

	araya::task<void> apply(araya::plugin_context&) override {
		throw std::runtime_error("injected failure (" + what_ + ")");
	}

	std::string what_;
};

std::unique_ptr<araya::plugin> make_bomb(araya::plugin_config const& config) {
	auto it = config.find("what");
	return std::make_unique<bomb_plugin>(it == config.end() ? "unknown" : it->second);
}

// -- descriptors --------------------------------------------------------

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};

static const araya::dependency_spec g_console_inject[]{
	{araya::service_id{"logger", 1}, true, {}},
	{araya::service_id{"timer", 1}, true, {}},
	{araya::service_id{"sessions", 1}, true, {}},
};

static const araya::dependency_spec g_watcher_inject[]{
	{araya::service_id{"demo.beacon", 1}, true, {}},
	{araya::service_id{"logger", 1}, true, {}},
};

static const araya::provision_spec g_beacon_prov[]{{araya::service_id{"demo.beacon", 1}}};
static const araya::provision_spec g_bomb_logger_prov[]{{araya::service_id{"logger", 1}}};
static const araya::provision_spec g_bomb_timer_prov[]{{araya::service_id{"timer", 1}}};
static const araya::provision_spec g_bomb_sessions_prov[]{{araya::service_id{"sessions", 1}}};

static const araya::plugin_descriptor g_console_desc{"console", g_console_inject, g_no_provs, &make_console};
static const araya::plugin_descriptor g_beacon_desc{"beacon", g_no_deps, g_beacon_prov, &make_beacon};
static const araya::plugin_descriptor g_watcher_desc{"watcher", g_watcher_inject, g_no_provs, &make_watcher};
static const araya::plugin_descriptor g_bomb_logger_desc{"bomb", g_no_deps, g_bomb_logger_prov, &make_bomb};
static const araya::plugin_descriptor g_bomb_timer_desc{"bomb", g_no_deps, g_bomb_timer_prov, &make_bomb};
static const araya::plugin_descriptor g_bomb_sessions_desc{"bomb", g_no_deps, g_bomb_sessions_prov, &make_bomb};

} // namespace

void init_demo_state(std::shared_ptr<demo_state> state) { g_state = std::move(state); }

demo_state& state() {
	if (!g_state)
		throw std::logic_error("demo state not initialized");
	return *g_state;
}

araya::plugin_descriptor const& console_descriptor() { return g_console_desc; }
araya::plugin_descriptor const& beacon_descriptor() { return g_beacon_desc; }
araya::plugin_descriptor const& watcher_descriptor() { return g_watcher_desc; }

araya::plugin_descriptor const& bomb_descriptor_for(std::string_view provided_key) {
	if (provided_key == "logger")
		return g_bomb_logger_desc;
	if (provided_key == "timer")
		return g_bomb_timer_desc;
	if (provided_key == "session")
		return g_bomb_sessions_desc;
	throw std::invalid_argument("no bomb provides '" + std::string(provided_key) + "'");
}

} // namespace araya::console_demo
