#include "commands.hpp"

#include "demo_plugins.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// The fiber-tree and desired-tree commands: ls, load, unload, reload,
// fail, avail - plus the status/reconcile helpers they share.
namespace araya::app {
namespace {

std::optional<std::string> status_of(std::string_view path, std::vector<araya::fiber_info> const& fibers) {
	for (auto const& f : fibers) {
		if (f.name == path)
			return std::string(path) + ": " + state_name(f.state) + (f.error ? " (" + error_text(f.error) + ")" : "");
	}
	return std::nullopt;
}

araya::task<void> print_status(app_context& ctx, line_sink const& out, std::string_view path) {
	auto fibers = co_await ctx.rt->fibers_async();
	auto status = status_of(path, fibers);
	out(status ? *status : std::string(path) + ": absent");
}

araya::task<void> reconcile_print(app_context& ctx, line_sink const& out, std::string_view touched) {
	co_await ctx.rt->reconcile(make_desired(ctx));
	co_await ctx.rt->wait_idle();
	co_await print_status(ctx, out, touched);
	if (touched != "console" && ctx.desired.contains("console"))
		co_await print_status(ctx, out, "console");
}

} // namespace

araya::task<void> cmd_ls(app_context& ctx, line_sink const& out, std::string const&) {
	auto fibers = co_await ctx.rt->fibers_async();
	out("fiber tree:");
	for (auto const& f : fibers) {
		out("  " + f.name + " (" + f.descriptor + ") " + state_name(f.state) +
			(f.error ? " error: " + error_text(f.error) : ""));
	}
	if (fibers.empty())
		out("  (empty)");
}

araya::task<void> cmd_load(app_context& ctx, line_sink const& out, std::string const& line) {
	std::istringstream is(line);
	std::string cmd;
	std::string name;
	is >> cmd >> name;
	if (name.empty()) {
		out("load: component name required");
		co_return;
	}
	auto* descriptor = real_descriptor(name);
	if (!descriptor) {
		out("unknown component '" + name + "'");
		co_return;
	}
	if (ctx.desired.contains(name)) {
		out(name + ": already loaded");
		co_return;
	}
	araya::plugin_config config = default_config(name);
	// Optional inline JSON config: load <component> {"key": ...}. Values
	// that are not strings ride along serialized.
	std::string tail;
	std::getline(is, tail);
	tail = trim(tail);
	if (!tail.empty()) {
		boost::system::error_code ec;
		auto value = boost::json::parse(tail, ec);
		if (ec || !value.is_object()) {
			out("load: trailing config must be a JSON object");
			co_return;
		}
		for (auto const& [key, entry] : value.as_object()) {
			if (entry.is_string())
				config[key] = std::string(entry.as_string());
			else
				config[key] = boost::json::serialize(entry);
		}
	}
	ctx.desired[name] = desired_entry{descriptor, std::move(config)};
	co_await reconcile_print(ctx, out, name);
}

araya::task<void> cmd_unload(app_context& ctx, line_sink const& out, std::string const& line) {
	std::istringstream is(line);
	std::string cmd;
	std::string name;
	is >> cmd >> name;
	if (name.empty()) {
		out("unload: component name required");
		co_return;
	}
	if (!ctx.desired.contains(name)) {
		out(name + ": not loaded");
		co_return;
	}
	ctx.desired.erase(name);
	co_await ctx.rt->reconcile(make_desired(ctx));
	co_await ctx.rt->wait_idle();
	out(name + ": retired");
	if (name != "console" && ctx.desired.contains("console"))
		co_await print_status(ctx, out, "console");
}

araya::task<void> cmd_reload(app_context& ctx, line_sink const& out, std::string const& line) {
	std::istringstream is(line);
	std::string cmd;
	std::string name;
	is >> cmd >> name;
	if (name.empty()) {
		out("reload: component name required");
		co_return;
	}
	if (!ctx.desired.contains(name)) {
		out(name + ": not loaded");
		co_return;
	}
	auto entry = ctx.desired.at(name);
	ctx.desired.erase(name);
	co_await ctx.rt->reconcile(make_desired(ctx));
	co_await ctx.rt->wait_idle();
	ctx.desired[name] = entry;
	co_await reconcile_print(ctx, out, name);
}

araya::task<void> cmd_fail(app_context& ctx, line_sink const& out, std::string const& line) {
	std::istringstream is(line);
	std::string cmd;
	std::string name;
	is >> cmd >> name;
	if (name.empty()) {
		out("fail: component name required");
		co_return;
	}
	if (name == "clear") {
		for (auto const& key : {"logger", "timer", "session"}) {
			auto it = ctx.desired.find(key);
			if (it != ctx.desired.end()) {
				it->second.descriptor = real_descriptor(key);
				it->second.config = default_config(key);
			}
		}
		co_await ctx.rt->reconcile(make_desired(ctx));
		co_await ctx.rt->wait_idle();
		out("fail: providers restored");
		co_await print_status(ctx, out, "console");
		co_return;
	}
	if (name != "logger" && name != "timer" && name != "session") {
		out("fail targets: logger, timer, session (or 'clear')");
		co_return;
	}
	auto it = ctx.desired.find(name);
	if (it == ctx.desired.end()) {
		out(name + ": not loaded");
		co_return;
	}
	it->second.descriptor = &araya::console_demo::bomb_descriptor_for(name);
	it->second.config = {{"what", name}};
	co_await reconcile_print(ctx, out, name);
}

araya::task<void> cmd_avail(app_context& ctx, line_sink const& out, std::string const& line) {
	std::istringstream is(line);
	std::string cmd;
	std::string action;
	is >> cmd >> action;
	if (!ctx.desired.contains("beacon")) {
		out("beacon: not loaded");
		co_return;
	}
	if (action == "wait") {
		// Retirement is the only way back to unavailable: retract the
		// beacon, clear its readiness, and remount - the availability
		// check runs once at provide-time, so the new binding publishes
		// unavailable and the watcher parks.
		araya::console_demo::state().beacon_ready.store(false, std::memory_order_release);
		auto entry = ctx.desired.at("beacon");
		ctx.desired.erase("beacon");
		co_await ctx.rt->reconcile(make_desired(ctx));
		co_await ctx.rt->wait_idle();
		ctx.desired["beacon"] = entry;
		co_await reconcile_print(ctx, out, "beacon");
		co_await print_status(ctx, out, "watcher");
	} else if (action == "ready") {
		araya::console_demo::state().beacon_ready.store(true, std::memory_order_release);
		ctx.rt->bus()->dispatch(araya::console_demo::beacon_ready_key, std::string("go"));
		co_await ctx.rt->wait_idle();
		co_await print_status(ctx, out, "watcher");
	} else {
		out("avail: wait | ready");
	}
}

} // namespace araya::app
