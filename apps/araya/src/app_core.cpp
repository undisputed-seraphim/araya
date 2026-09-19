#include "app_core.hpp"

#include "demo_plugins.hpp"

#include "araya/logger/logger.hpp"
#include "araya/persistence/persistence.hpp"
#include "araya/session/events.hpp"
#include "araya/session/store.hpp"
#include "araya/timer/timer.hpp"

#include <boost/asio/use_awaitable.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace araya::app {
namespace {

using namespace std::chrono_literals;

using araya::logger::logger_key;
using araya::logger::logger_service;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::timer::timer_key;
using araya::timer::timer_service;

// -- helpers -------------------------------------------------------------

std::string trim(std::string_view text) {
	auto first = text.find_first_not_of(" \t");
	if (first == std::string_view::npos)
		return {};
	auto last = text.find_last_not_of(" \t");
	return std::string(text.substr(first, last - first + 1));
}

boost::json::value user_message(std::string_view id, std::string_view text) {
	return {
		{"id", std::string(id)},
		{"role", "user"},
		{"content", boost::json::array{{{"type", "text"}, {"text", std::string(text)}}}},
	};
}

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

araya::logger::log_level parse_level(std::string_view word) {
	if (word == "error")
		return araya::logger::log_level::error;
	if (word == "warn")
		return araya::logger::log_level::warn;
	if (word == "info")
		return araya::logger::log_level::info;
	if (word == "debug")
		return araya::logger::log_level::debug;
	throw std::invalid_argument("level is one of error, warn, info, debug");
}

// -- commands (on the control strand) -------------------------------------

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
	ctx.desired[name] = desired_entry{descriptor, default_config(name)};
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

araya::task<void> cmd_session(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		std::string sub;
		is >> cmd >> sub;

		auto store_of = [&]() -> std::shared_ptr<session_store> {
			return ctx.rt->root_context().require<session_store>(sessions_key).shared();
		};

		if (sub == "new") {
			std::string id;
			is >> id;
			auto store = store_of();
			auto root_ctx = ctx.rt->root_context();
			araya::session::session_id sid = id.empty() ? store->mint_id() : araya::session::session_id{std::move(id)};
			(void)store->create(root_ctx, sid, {});
			ctx.current = sid;
			out("session: created " + sid.value);
		} else if (sub == "switch") {
			std::string id;
			is >> id;
			if (id.empty()) {
				out("session: switch needs an id");
			} else {
				auto store = store_of();
				araya::session::session_id target{id};
				if (!store->get(target)) {
					out("session: unknown id '" + target.value + "'");
				} else {
					ctx.current = std::move(target);
					out("session: switched to " + ctx.current->value);
				}
			}
		} else if (sub == "list") {
			auto store = store_of();
			auto ids = store->list();
			if (ids.empty()) {
				out("session: none live");
			} else {
				for (auto const& id : ids) {
					bool is_current = ctx.current && ctx.current->value == id.value;
					out(std::string(is_current ? "* " : "  ") + id.value);
				}
			}
		} else if (sub == "append") {
			std::string text;
			std::getline(is, text);
			text = trim(text);
			if (!ctx.current) {
				out("session: no current session");
			} else {
				auto store = store_of();
				auto s = store->get(*ctx.current);
				if (!s) {
					out("session: current session was disposed");
					ctx.current.reset();
				} else {
					auto seq = s->append("user/message", user_message("u" + std::to_string(s->log().size()), text));
					out("session: appended seq " + std::to_string(seq));
				}
			}
		} else if (sub == "replace") {
			std::int64_t start = 0;
			std::int64_t end = 0;
			std::string text;
			is >> start >> end;
			std::getline(is, text);
			text = trim(text);
			if (!is || text.empty()) {
				out("session: usage: session replace <start> <end> <text>");
			} else if (!ctx.current) {
				out("session: no current session");
			} else {
				auto store = store_of();
				auto s = store->get(*ctx.current);
				if (!s) {
					out("session: current session was disposed");
					ctx.current.reset();
				} else {
					boost::json::value replace = {
						{"start_seq", start},
						{"end_seq", end},
						{"message", user_message("r" + std::to_string(s->log().size()), text)},
					};
					auto seq = s->append("surface/replace", std::move(replace));
					out("session: replaced [" + std::to_string(start) + ", " + std::to_string(end) + ") at seq " +
						std::to_string(seq));
				}
			}
		} else if (sub == "fork") {
			std::string parent;
			std::string child;
			is >> parent >> child;
			if (parent.empty()) {
				out("session: usage: session fork <parent> [child]");
			} else {
				auto store = store_of();
				auto root_ctx = ctx.rt->root_context();
				araya::session::session_id pid{parent};
				araya::session::session_id cid =
					child.empty() ? araya::session::session_id{} : araya::session::session_id{std::move(child)};
				auto s = store->fork(root_ctx, pid, cid);
				ctx.current = s->id();
				out("session: forked " + s->id().value + " from " + pid.value + " (" +
					std::to_string(s->inherited_event_count()) + " inherited events)");
			}
		} else if (sub == "show") {
			if (!ctx.current) {
				out("session: no current session");
			} else {
				auto store = store_of();
				auto s = store->get(*ctx.current);
				if (!s) {
					out("session: current session was disposed");
					ctx.current.reset();
				} else {
					for (auto const& m : s->surface().messages())
						out("  " + role_name(m.role) + ": " + boost::json::serialize(m.content));
				}
			}
		} else if (sub == "save") {
			if (!ctx.current) {
				out("session: no current session");
			} else {
				auto store = store_of();
				auto s = store->get(*ctx.current);
				if (!s) {
					out("session: current session was disposed");
					ctx.current.reset();
				} else {
					// The durability barrier: the persistence plugin's flush
					// listener drains and fsyncs before this completes.
					co_await s->flush();
					out("session: saved " + s->id().value);
				}
			}
		} else if (sub == "load" || sub == "load-all") {
			bool all = sub == "load-all";
			std::string id;
			is >> id;
			if (!all && id.empty()) {
				out("session: load needs an id (or use 'session load-all')");
			} else {
				auto store = store_of();
				auto backend =
					ctx.rt->root_context()
						.require<araya::persistence::session_persistence>(araya::persistence::persistence_key)
						.shared();
				std::vector<araya::session::session_id> targets;
				if (all) {
					targets = backend->list();
				} else {
					targets.push_back(araya::session::session_id{std::move(id)});
				}
				if (targets.empty()) {
					out("session: nothing on disk");
				} else {
					for (auto& target : targets) {
						if (store->get(target)) {
							out("session: " + target.value + " already live");
							continue;
						}
						auto stored = backend->read(target);
						if (!stored) {
							out("session: " + target.value + " not on disk");
							continue;
						}
						auto const event_count = stored->events.size();
						auto s = store->prepare(
							target,
							araya::session::create_session_options{
								.seed = std::move(stored->events),
								.inherited_event_count = stored->events.size(),
								.cwd = stored->header.cwd,
								.parent_session = stored->header.parent_session,
								.created_at = stored->header.created_at,
								.is_seeded = false,
								.origin = stored->header.origin,
								.delegation_depth = stored->header.delegation_depth,
								.agent_preset = stored->header.agent_preset,
							});
						store->enter(s);
						store->announce(*s);
						if (!ctx.current)
							ctx.current = target;
						out("session: restored " + target.value + " (" + std::to_string(event_count) + " events)");
					}
				}
			}
		} else if (sub == "close") {
			std::string id;
			is >> id;
			auto target = id.empty() ? *ctx.current : araya::session::session_id{std::move(id)};
			auto store = store_of();
			if (!store->dispose(target)) {
				out("session: unknown id '" + target.value + "'");
			} else {
				if (ctx.current && ctx.current->value == target.value)
					ctx.current.reset();
				out("session: closed " + target.value);
			}
		} else {
			out("session: new | switch | list | append | replace | fork | show | save | load | load-all | close");
		}
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
	co_return;
}

araya::task<void> cmd_timer(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		std::string sub;
		is >> cmd >> sub;

		if (sub == "in" || sub == "every") {
			double seconds = 0.0;
			is >> seconds;
			std::string text;
			std::getline(is, text);
			text = trim(text);
			if (!is || text.empty()) {
				out("timer: usage: timer " + sub + " <seconds> <text>");
			} else {
				auto root_ctx = ctx.rt->root_context();
				auto timer = root_ctx.require<timer_service>(timer_key).shared();
				auto delay =
					std::chrono::duration_cast<timer_service::duration>(std::chrono::duration<double>(seconds));
				if (sub == "in") {
					(void)timer->timeout(root_ctx, delay, [out, text] { out("timer: " + text); });
					out("timer: armed");
				} else {
					auto reg = timer->interval(root_ctx, delay, [out, text] {
						out("timer: " + text);
						return true;
					});
					ctx.intervals.push_back(std::move(reg));
					out("timer: interval armed");
				}
			}
		} else if (sub == "cancel") {
			for (auto& reg : ctx.intervals)
				reg.release();
			ctx.intervals.clear();
			out("timer: intervals cancelled");
		} else {
			out("timer: in | every | cancel");
		}
	} catch (std::exception const& e) {
		out(std::string("timer: ") + e.what());
	}
	co_return;
}

araya::task<void> cmd_log(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		std::string sub;
		is >> cmd >> sub;

		auto logger = ctx.rt->root_context().require<logger_service>(logger_key).shared();
		if (sub == "level") {
			std::string level;
			is >> level;
			if (level.empty()) {
				out("log: usage: log level <level>");
			} else {
				logger->set_level(parse_level(level));
				out("log: level set");
			}
		} else {
			std::string text;
			std::getline(is, text);
			text = trim(text);
			if (text.empty()) {
				out("log: usage: log <level> <text>");
			} else {
				auto nl = logger->named("main");
				switch (parse_level(sub)) {
				case araya::logger::log_level::error:
					nl.error("{}", text);
					break;
				case araya::logger::log_level::warn:
					nl.warn("{}", text);
					break;
				case araya::logger::log_level::info:
					nl.info("{}", text);
					break;
				case araya::logger::log_level::debug:
					nl.debug("{}", text);
					break;
				}
				out("log: submitted");
			}
		}
	} catch (std::exception const& e) {
		out(std::string("log: ") + e.what());
	}
	co_return;
}

using command_fn = araya::task<void> (*)(app_context&, line_sink const&, std::string const&);

struct command_entry {
	std::string_view name;
	std::string_view usage;
	std::string_view summary;
	command_fn run;
};

constexpr command_entry g_commands[]{
	{"ls", "ls", "print the fiber tree", &cmd_ls},
	{"load", "load <component>", "add logger | timer | session | persistence | console | beacon | watcher", &cmd_load},
	{"unload", "unload <component>", "retire it (watch the cascade)", &cmd_unload},
	{"reload", "reload <component>", "retire and remount it", &cmd_reload},
	{"fail", "fail <component>", "swap in a crashing provider (logger | timer | session)", &cmd_fail},
	{"avail", "avail wait | ready", "park the watcher on the beacon, then promote it", &cmd_avail},
	{"session",
	 "session ...",
	 "new | switch | list | append | replace | fork | show | save | load | close",
	 &cmd_session},
	{"timer", "timer ...", "in <sec> <text> | every <sec> <text> | cancel", &cmd_timer},
	{"log", "log ...", "level <level> | <level> <text>", &cmd_log},
};

} // namespace

// -- public API -----------------------------------------------------------

std::shared_ptr<araya::plugin_descriptor> borrow(araya::plugin_descriptor const& d) {
	return std::shared_ptr<araya::plugin_descriptor>(
		const_cast<araya::plugin_descriptor*>(&d), [](araya::plugin_descriptor*) {});
}

std::vector<araya::desired_component> make_desired(app_context& ctx) {
	std::vector<araya::desired_component> result;
	result.reserve(ctx.desired.size());
	for (auto const& [path, entry] : ctx.desired)
		result.push_back(araya::desired_component{
			path,
			araya::component_spec{borrow(*entry.descriptor), entry.config, nullptr, path, {}},
		});
	return result;
}

araya::plugin_descriptor const* real_descriptor(std::string_view name) {
	if (name == "logger")
		return &araya::logger::plugin_descriptor();
	if (name == "timer")
		return &araya::timer::plugin_descriptor();
	if (name == "session")
		return &araya::session::plugin_descriptor();
	if (name == "persistence")
		return &araya::persistence::plugin_descriptor();
	if (name == "console")
		return &araya::console_demo::console_descriptor();
	if (name == "beacon")
		return &araya::console_demo::beacon_descriptor();
	if (name == "watcher")
		return &araya::console_demo::watcher_descriptor();
	return nullptr;
}

araya::plugin_config default_config(std::string_view name) {
	if (name == "logger")
		return {{"name", "araya"}, {"level", "info"}};
	if (name == "persistence")
		return {{"root", "araya-sessions"}};
	return {};
}

char const* state_name(araya::fiber_state s) {
	switch (s) {
	case araya::fiber_state::inactive:
		return "inactive";
	case araya::fiber_state::loading:
		return "loading";
	case araya::fiber_state::active:
		return "active";
	case araya::fiber_state::unloading:
		return "unloading";
	}
	return "?";
}

std::string error_text(std::exception_ptr ep) {
	if (!ep)
		return {};
	try {
		std::rethrow_exception(ep);
	} catch (std::exception const& e) {
		return e.what();
	} catch (...) {
		return "unknown error";
	}
}

std::string role_name(araya::session::message_role role) {
	switch (role) {
	case araya::session::message_role::system:
		return "system";
	case araya::session::message_role::user:
		return "user";
	case araya::session::message_role::assistant:
		return "assistant";
	case araya::session::message_role::tool_result:
		return "tool";
	}
	return "?";
}

std::string cwd_branch_line(std::string const& cwd) {
	std::string shown = cwd;
	if (auto home = std::getenv("HOME"); home && !shown.rfind(home, 0)) {
		shown = "~" + shown.substr(std::strlen(home));
	}
	std::string branch;
	std::string command = "git -C \"" + cwd + "\" symbolic-ref --short HEAD 2>/dev/null";
	if (FILE* pipe = ::popen(command.c_str(), "r")) {
		char buffer[128] = {};
		if (std::fgets(buffer, sizeof buffer, pipe)) {
			branch = buffer;
			while (!branch.empty() && (branch.back() == '\n' || branch.back() == '\r'))
				branch.pop_back();
		}
		::pclose(pipe);
	}
	return branch.empty() ? shown : shown + ":" + branch;
}

araya::task<void> boot(app_context& ctx, line_sink const& out) {
	ctx.desired["logger"] = desired_entry{&araya::logger::plugin_descriptor(), {{"name", "araya"}, {"level", "info"}}};
	ctx.desired["timer"] = desired_entry{&araya::timer::plugin_descriptor(), {}};
	ctx.desired["session"] = desired_entry{&araya::session::plugin_descriptor(), {}};
	ctx.desired["persistence"] = desired_entry{&araya::persistence::plugin_descriptor(), {{"root", "araya-sessions"}}};
	ctx.desired["beacon"] = desired_entry{&araya::console_demo::beacon_descriptor(), {}};
	ctx.desired["watcher"] = desired_entry{&araya::console_demo::watcher_descriptor(), {}};
	ctx.desired["console"] = desired_entry{&araya::console_demo::console_descriptor(), {}};

	co_await ctx.rt->reconcile(make_desired(ctx));
	co_await ctx.rt->wait_idle();

	std::error_code cwd_ec;
	ctx.cwd = std::filesystem::current_path(cwd_ec).string();
	if (cwd_ec)
		ctx.cwd = "?";
	ctx.cwd_branch = cwd_branch_line(ctx.cwd);

	// The live feed: host-owned listeners on the session firehose. They
	// live until the bus dies with the runtime, and write as events land.
	co_await ctx.rt->run_on_strand([&] {
		auto* bus = ctx.rt->bus().get();
		bus->add_listener(
			araya::session::created_key,
			[out](araya::session::session_created_msg const& m) { out("event: session/created " + m.s->id().value); },
			0);
		bus->add_listener(
			araya::session::disposed_key,
			[out](araya::session::session_disposed_msg const& m) { out("event: session/disposed " + m.id.value); },
			0);
		bus->add_listener(
			araya::session::appended_key,
			[out](araya::session::session_appended_msg const& m) {
				out("event: session/event " + m.id.value + " seq " + std::to_string(m.event.seq) + " type " +
					m.event.type);
			},
			0);
	});
}

araya::task<void> dispatch(app_context& ctx, line_sink const& out, std::string_view line) {
	auto trimmed = trim(line);
	if (trimmed.empty())
		co_return;
	std::istringstream is(trimmed);
	std::string name;
	is >> name;

	if (name == "help") {
		out(help_text());
		co_return;
	}
	for (auto const& entry : g_commands) {
		if (entry.name == name) {
			co_await entry.run(ctx, out, trimmed);
			co_return;
		}
	}
	out("unknown command '" + name + "' (try help)");
}

std::string help_text() {
	std::size_t usage_width = 0;
	for (auto const& entry : g_commands)
		usage_width = std::max(usage_width, entry.usage.size());
	std::string text = "commands:\n";
	for (auto const& entry : g_commands) {
		text += "  " + std::string(entry.usage);
		text += std::string(usage_width - entry.usage.size() + 2, ' ');
		text += std::string(entry.summary) + "\n";
	}
	text += "  help                    this text";
	return text;
}

} // namespace araya::app
