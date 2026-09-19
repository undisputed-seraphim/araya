#include "demo_plugins.hpp"

#include "araya/fiber_handle.hpp"
#include "araya/logger/logger.hpp"
#include "araya/persistence/persistence.hpp"
#include "araya/runtime.hpp"
#include "araya/session/events.hpp"
#include "araya/session/store.hpp"
#include "araya/timer/timer.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The console application: a thin host that mounts the first-party
// plugins plus the demo components and drives an interactive shell over
// them. Every command goes through the public engine surface - the
// desired-tree reconcile, the control-strand accessors, and the services
// themselves - so the demo is the engine's own semantics, and every
// outage a command reports is the availability machinery working.

namespace {

using namespace std::chrono_literals;

using araya::logger::logger_key;
using araya::logger::logger_service;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::timer::timer_key;
using araya::timer::timer_service;

void out(std::string_view text) { std::cout << text << '\n' << std::flush; }

void prompt() { std::cout << "araya> " << std::flush; }

// A no-op-deleter view over a static descriptor: the same pattern the
// engine tests use. First-party and demo descriptors are static storage.
std::shared_ptr<araya::plugin_descriptor> borrow(araya::plugin_descriptor const& d) {
	return std::shared_ptr<araya::plugin_descriptor>(
		const_cast<araya::plugin_descriptor*>(&d), [](araya::plugin_descriptor*) {});
}

struct desired_entry {
	araya::plugin_descriptor const* descriptor = nullptr;
	araya::plugin_config config;
};

struct app {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	std::map<std::string, desired_entry> desired;
	std::optional<araya::session::session_id> current;
	std::vector<araya::registration> intervals;
	bool quitting = false;
};

std::vector<araya::desired_component> make_desired(app& a) {
	std::vector<araya::desired_component> result;
	result.reserve(a.desired.size());
	for (auto const& [path, entry] : a.desired)
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

const char* state_name(araya::fiber_state s) {
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

std::optional<std::string> status_of(std::string_view path, std::vector<araya::fiber_info> const& fibers) {
	for (auto const& f : fibers) {
		if (f.name == path)
			return std::string(path) + ": " + state_name(f.state) + (f.error ? " (" + error_text(f.error) + ")" : "");
	}
	return std::nullopt;
}

araya::task<void> print_status(app& a, std::string_view path) {
	auto fibers = co_await a.rt->fibers_async();
	auto status = status_of(path, fibers);
	out(status ? *status : std::string(path) + ": absent");
}

araya::task<void> reconcile_print(app& a, std::string_view touched) {
	co_await a.rt->reconcile(make_desired(a));
	co_await a.rt->wait_idle();
	co_await print_status(a, touched);
	if (touched != "console" && a.desired.contains("console"))
		co_await print_status(a, "console");
}

// -- commands -----------------------------------------------------------

araya::task<void> cmd_ls(app& a) {
	auto fibers = co_await a.rt->fibers_async();
	out("fiber tree:");
	for (auto const& f : fibers) {
		out("  " + f.name + " (" + f.descriptor + ") " + state_name(f.state) +
			(f.error ? " error: " + error_text(f.error) : ""));
	}
	if (fibers.empty())
		out("  (empty)");
}

araya::task<void> cmd_load(app& a, std::string_view name) {
	auto* descriptor = real_descriptor(name);
	if (!descriptor) {
		out("unknown component '" + std::string(name) + "'");
		co_return;
	}
	if (a.desired.contains(std::string(name))) {
		out(std::string(name) + ": already loaded");
		co_return;
	}
	a.desired[std::string(name)] = desired_entry{descriptor, default_config(name)};
	co_await reconcile_print(a, name);
}

araya::task<void> cmd_unload(app& a, std::string_view name) {
	if (!a.desired.contains(std::string(name))) {
		out(std::string(name) + ": not loaded");
		co_return;
	}
	a.desired.erase(std::string(name));
	co_await a.rt->reconcile(make_desired(a));
	co_await a.rt->wait_idle();
	out(std::string(name) + ": retired");
	if (name != "console" && a.desired.contains("console"))
		co_await print_status(a, "console");
}

araya::task<void> cmd_reload(app& a, std::string_view name) {
	if (!a.desired.contains(std::string(name))) {
		out(std::string(name) + ": not loaded");
		co_return;
	}
	auto entry = a.desired.at(std::string(name));
	a.desired.erase(std::string(name));
	co_await a.rt->reconcile(make_desired(a));
	co_await a.rt->wait_idle();
	a.desired[std::string(name)] = entry;
	co_await reconcile_print(a, name);
}

araya::task<void> cmd_fail(app& a, std::string_view name) {
	if (name == "clear") {
		for (auto const& key : {"logger", "timer", "session"}) {
			auto it = a.desired.find(key);
			if (it != a.desired.end()) {
				it->second.descriptor = real_descriptor(key);
				it->second.config = default_config(key);
			}
		}
		co_await a.rt->reconcile(make_desired(a));
		co_await a.rt->wait_idle();
		out("fail: providers restored");
		co_await print_status(a, "console");
		co_return;
	}
	if (name != "logger" && name != "timer" && name != "session") {
		out("fail targets: logger, timer, session (or 'clear')");
		co_return;
	}
	auto it = a.desired.find(std::string(name));
	if (it == a.desired.end()) {
		out(std::string(name) + ": not loaded");
		co_return;
	}
	it->second.descriptor = &araya::console_demo::bomb_descriptor_for(name);
	it->second.config = {{"what", std::string(name)}};
	co_await reconcile_print(a, name);
}

araya::task<void> cmd_avail(app& a, std::string_view action) {
	if (!a.desired.contains("beacon")) {
		out("beacon: not loaded");
		co_return;
	}
	if (action == "wait") {
		// Retirement is the only way back to unavailable: retract the
		// beacon, clear its readiness, and remount - the availability
		// check runs once at provide-time, so the new binding publishes
		// unavailable and the watcher parks.
		araya::console_demo::state().beacon_ready.store(false, std::memory_order_release);
		auto entry = a.desired.at("beacon");
		a.desired.erase("beacon");
		co_await a.rt->reconcile(make_desired(a));
		co_await a.rt->wait_idle();
		a.desired["beacon"] = entry;
		co_await reconcile_print(a, "beacon");
		co_await print_status(a, "watcher");
	} else if (action == "ready") {
		araya::console_demo::state().beacon_ready.store(true, std::memory_order_release);
		co_await a.rt->run_on_strand(
			[&] { a.rt->bus()->dispatch(araya::console_demo::beacon_ready_key, std::string("go")); });
		co_await a.rt->wait_idle();
		co_await print_status(a, "watcher");
	} else {
		out("avail: wait | ready");
	}
}

araya::task<void> cmd_session_new(app& a, std::optional<std::string> id_arg) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			araya::session::session_id id =
				id_arg && !id_arg->empty() ? araya::session::session_id{*id_arg} : store->mint_id();
			(void)store->create(root_ctx, id, {});
			a.current = id;
			out("session: created " + id.value);
		});
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_session_switch(app& a, std::string_view id) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			araya::session::session_id target{std::string(id)};
			if (!store->get(target)) {
				out("session: unknown id '" + target.value + "'");
				return;
			}
			a.current = std::move(target);
			out("session: switched to " + a.current->value);
		});
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_session_list(app& a) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			auto ids = store->list();
			if (ids.empty()) {
				out("session: none live");
				return;
			}
			for (auto const& id : ids) {
				bool is_current = a.current && a.current->value == id.value;
				out(std::string(is_current ? "* " : "  ") + id.value);
			}
		});
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_session_append(app& a, std::string_view text) {
	try {
		co_await a.rt->run_on_strand([&] {
			if (!a.current) {
				out("session: no current session");
				return;
			}
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			auto s = store->get(*a.current);
			if (!s) {
				out("session: current session was disposed");
				a.current.reset();
				return;
			}
			boost::json::value message = {
				{"id", "u" + std::to_string(s->log().size())},
				{"role", "user"},
				{"content", boost::json::array{{{"type", "text"}, {"text", std::string(text)}}}},
			};
			auto seq = s->append("user/message", std::move(message));
			out("session: appended seq " + std::to_string(seq));
		});
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_session_show(app& a) {
	try {
		co_await a.rt->run_on_strand([&] {
			if (!a.current) {
				out("session: no current session");
				return;
			}
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			auto s = store->get(*a.current);
			if (!s) {
				out("session: current session was disposed");
				a.current.reset();
				return;
			}
			for (auto const& m : s->surface().messages())
				out("  " + role_name(m.role) + ": " + boost::json::serialize(m.content));
		});
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_session_close(app& a, std::optional<std::string> id_arg) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto target = id_arg ? araya::session::session_id{*id_arg} : *a.current;
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			if (!store->dispose(target)) {
				out("session: unknown id '" + target.value + "'");
				return;
			}
			if (a.current && a.current->value == target.value)
				a.current.reset();
			out("session: closed " + target.value);
		});
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_session_save(app& a) {
	try {
		std::shared_ptr<araya::session::session> s;
		co_await a.rt->run_on_strand([&] {
			if (!a.current) {
				out("session: no current session");
				return;
			}
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			s = store->get(*a.current);
			if (!s) {
				out("session: current session was disposed");
				a.current.reset();
			}
		});
		if (!s)
			co_return;
		// The durability barrier: the persistence plugin's flush listener
		// drains and fsyncs, and this await completes only after it has.
		co_await boost::asio::co_spawn(a.rt->bus()->executor(), s->flush(), boost::asio::use_awaitable);
		out("session: saved " + s->id().value);
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_session_load(app& a, std::string_view id, bool all) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			auto backend =
				root_ctx.require<araya::persistence::session_persistence>(araya::persistence::persistence_key);

			std::vector<araya::session::session_id> targets;
			if (all) {
				targets = backend->list();
			} else {
				targets.push_back(araya::session::session_id{std::string(id)});
			}
			if (targets.empty()) {
				out("session: nothing on disk");
				return;
			}
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
				if (!a.current)
					a.current = target;
				out("session: restored " + target.value + " (" + std::to_string(event_count) + " events)");
			}
		});
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_session_fork(app& a, std::string_view parent, std::optional<std::string> child) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			araya::session::session_id pid{std::string(parent)};
			araya::session::session_id cid =
				child && !child->empty() ? araya::session::session_id{*child} : araya::session::session_id{};
			auto s = store->fork(root_ctx, pid, cid);
			a.current = s->id();
			out("session: forked " + s->id().value + " from " + pid.value + " (" +
				std::to_string(s->inherited_event_count()) + " inherited events)");
		});
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_session_replace(app& a, std::int64_t start, std::int64_t end, std::string_view text) {
	try {
		co_await a.rt->run_on_strand([&] {
			if (!a.current) {
				out("session: no current session");
				return;
			}
			auto root_ctx = a.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			auto s = store->get(*a.current);
			if (!s) {
				out("session: current session was disposed");
				a.current.reset();
				return;
			}
			boost::json::value message = {
				{"id", "r" + std::to_string(s->log().size())},
				{"role", "user"},
				{"content", boost::json::array{{{"type", "text"}, {"text", std::string(text)}}}},
			};
			boost::json::value replace = {
				{"start_seq", start},
				{"end_seq", end},
				{"message", std::move(message)},
			};
			auto seq = s->append("surface/replace", std::move(replace));
			out("session: replaced [" + std::to_string(start) + ", " + std::to_string(end) + ") at seq " +
				std::to_string(seq));
		});
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
}

araya::task<void> cmd_timer_in(app& a, double seconds, std::string_view text) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto root_ctx = a.rt->root_context();
			auto timer = root_ctx.require<timer_service>(timer_key);
			auto delay = std::chrono::duration_cast<timer_service::duration>(std::chrono::duration<double>(seconds));
			std::string what(text);
			(void)timer->timeout(root_ctx, delay, [what] { out("timer: " + what); });
			out("timer: armed");
		});
	} catch (std::exception const& e) {
		out(std::string("timer: ") + e.what());
	}
}

araya::task<void> cmd_timer_every(app& a, double seconds, std::string_view text) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto root_ctx = a.rt->root_context();
			auto timer = root_ctx.require<timer_service>(timer_key);
			auto delay = std::chrono::duration_cast<timer_service::duration>(std::chrono::duration<double>(seconds));
			std::string what(text);
			auto reg = timer->interval(root_ctx, delay, [what] {
				out("timer: " + what);
				return true;
			});
			a.intervals.push_back(std::move(reg));
			out("timer: interval armed");
		});
	} catch (std::exception const& e) {
		out(std::string("timer: ") + e.what());
	}
}

void cmd_timer_cancel(app& a) {
	for (auto& reg : a.intervals)
		reg.release();
	a.intervals.clear();
	out("timer: intervals cancelled");
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

araya::task<void> cmd_log(app& a, std::string_view level, std::string_view text) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto root_ctx = a.rt->root_context();
			auto logger = root_ctx.require<logger_service>(logger_key);
			auto nl = logger->named("main");
			std::string what(text);
			switch (parse_level(level)) {
			case araya::logger::log_level::error:
				nl.error("{}", what);
				break;
			case araya::logger::log_level::warn:
				nl.warn("{}", what);
				break;
			case araya::logger::log_level::info:
				nl.info("{}", what);
				break;
			case araya::logger::log_level::debug:
				nl.debug("{}", what);
				break;
			}
			out("log: submitted");
		});
	} catch (std::exception const& e) {
		out(std::string("log: ") + e.what());
	}
}

araya::task<void> cmd_log_level(app& a, std::string_view level) {
	try {
		co_await a.rt->run_on_strand([&] {
			auto root_ctx = a.rt->root_context();
			auto logger = root_ctx.require<logger_service>(logger_key);
			logger->set_level(parse_level(level));
			out("log: level set");
		});
	} catch (std::exception const& e) {
		out(std::string("log: ") + e.what());
	}
}

araya::task<void> cmd_sleep(app& a, std::int64_t ms) {
	boost::asio::steady_timer timer(a.io, std::chrono::milliseconds(ms));
	co_await timer.async_wait(boost::asio::use_awaitable);
}

constexpr std::string_view g_help = R"(commands:
  ls                      print the fiber tree
  load <component>        add logger | timer | session | persistence | console | beacon | watcher
  unload <component>      retire it (watch the cascade)
  reload <component>      retire and remount it
  fail <component>        swap in a crashing provider (logger | timer | session)
  fail clear              restore the real providers
  avail wait | ready      park the watcher on the beacon, then promote it
  session new [id]        create a session (current = it)
  session switch <id>     make another session current
  session list            the sessions live in the store
  session append <text>   append a user message to the current session
  session replace <s> <e> <text>  splice [s, e) into one message
  session fork <parent> [child]   fork a session from an earlier cut
  session show            print the folded message surface
  session save            flush the current session through the persistence barrier
  session load <id>       restore a persisted session from disk
  session load-all        restore every session found on disk
  session close [id]      dispose a session
  timer in <sec> <text>   fire a one-shot reminder
  timer every <sec> <text>  fire a repeating reminder
  timer cancel            stop all repeating reminders
  log <level> <text>      write a line through the logger service
  log level <level>       change the logger threshold (error, warn, info, debug)
  sleep <ms>              wait (script mode)
  help                    this text
  quit                    retire everything and exit)";

// -- the shell ----------------------------------------------------------

araya::task<void> exec_line(app& a, std::string line) {
	auto first = line.find_first_not_of(" \t");
	if (first == std::string::npos)
		co_return;
	auto last = line.find_last_not_of(" \t");
	line = line.substr(first, last - first + 1);

	std::istringstream is(line);
	std::string cmd;
	is >> cmd;

	if (cmd == "help") {
		out(g_help);
	} else if (cmd == "ls") {
		co_await cmd_ls(a);
	} else if (cmd == "load") {
		std::string name;
		is >> name;
		if (name.empty()) {
			out("load: component name required");
		} else {
			co_await cmd_load(a, name);
		}
	} else if (cmd == "unload") {
		std::string name;
		is >> name;
		if (name.empty()) {
			out("unload: component name required");
		} else {
			co_await cmd_unload(a, name);
		}
	} else if (cmd == "reload") {
		std::string name;
		is >> name;
		if (name.empty()) {
			out("reload: component name required");
		} else {
			co_await cmd_reload(a, name);
		}
	} else if (cmd == "fail") {
		std::string name;
		is >> name;
		if (name.empty()) {
			out("fail: component name required");
		} else {
			co_await cmd_fail(a, name);
		}
	} else if (cmd == "avail") {
		std::string action;
		is >> action;
		co_await cmd_avail(a, action);
	} else if (cmd == "session") {
		std::string sub;
		is >> sub;
		if (sub == "new") {
			std::string id;
			is >> id;
			co_await cmd_session_new(a, id.empty() ? std::nullopt : std::optional<std::string>(id));
		} else if (sub == "switch") {
			std::string id;
			is >> id;
			if (id.empty()) {
				out("session: switch needs an id");
			} else {
				co_await cmd_session_switch(a, id);
			}
		} else if (sub == "list") {
			co_await cmd_session_list(a);
		} else if (sub == "append") {
			std::string text;
			std::getline(is, text);
			auto t = text.find_first_not_of(' ');
			if (t != std::string::npos)
				text = text.substr(t);
			co_await cmd_session_append(a, text);
		} else if (sub == "replace") {
			std::int64_t start = 0;
			std::int64_t end = 0;
			std::string text;
			is >> start >> end;
			std::getline(is, text);
			auto t = text.find_first_not_of(' ');
			if (t != std::string::npos)
				text = text.substr(t);
			if (!is || text.empty()) {
				out("session: usage: session replace <start> <end> <text>");
			} else {
				co_await cmd_session_replace(a, start, end, text);
			}
		} else if (sub == "fork") {
			std::string parent;
			std::string child;
			is >> parent >> child;
			if (parent.empty()) {
				out("session: usage: session fork <parent> [child]");
			} else {
				co_await cmd_session_fork(a, parent, child.empty() ? std::nullopt : std::optional<std::string>(child));
			}
		} else if (sub == "show") {
			co_await cmd_session_show(a);
		} else if (sub == "save") {
			co_await cmd_session_save(a);
		} else if (sub == "load") {
			std::string id;
			is >> id;
			if (id.empty()) {
				out("session: load needs an id (or use 'session load-all')");
			} else {
				co_await cmd_session_load(a, id, false);
			}
		} else if (sub == "load-all") {
			co_await cmd_session_load(a, {}, true);
		} else if (sub == "close") {
			std::string id;
			is >> id;
			co_await cmd_session_close(a, id.empty() ? std::nullopt : std::optional<std::string>(id));
		} else {
			out("session: new | switch | list | append | replace | fork | show | save | load | load-all | close");
		}
	} else if (cmd == "timer") {
		std::string sub;
		is >> sub;
		if (sub == "in" || sub == "every") {
			double seconds = 0.0;
			is >> seconds;
			std::string text;
			std::getline(is, text);
			auto t = text.find_first_not_of(' ');
			if (t != std::string::npos)
				text = text.substr(t);
			if (!is || text.empty()) {
				out("timer: usage: timer " + sub + " <seconds> <text>");
			} else if (sub == "in") {
				co_await cmd_timer_in(a, seconds, text);
			} else {
				co_await cmd_timer_every(a, seconds, text);
			}
		} else if (sub == "cancel") {
			cmd_timer_cancel(a);
		} else {
			out("timer: in | every | cancel");
		}
	} else if (cmd == "log") {
		std::string sub;
		is >> sub;
		if (sub == "level") {
			std::string level;
			is >> level;
			if (level.empty()) {
				out("log: usage: log level <level>");
			} else {
				co_await cmd_log_level(a, level);
			}
		} else {
			std::string text;
			std::getline(is, text);
			auto t = text.find_first_not_of(' ');
			if (t != std::string::npos)
				text = text.substr(t);
			if (text.empty()) {
				out("log: usage: log <level> <text>");
			} else {
				co_await cmd_log(a, sub, text);
			}
		}
	} else if (cmd == "sleep") {
		std::int64_t ms = 0;
		is >> ms;
		if (!is || ms < 0) {
			out("sleep: usage: sleep <milliseconds>");
		} else {
			co_await cmd_sleep(a, ms);
		}
	} else if (cmd == "quit") {
		a.quitting = true;
		out("quit: retiring the tree");
	} else {
		out("unknown command '" + cmd + "' (try help)");
	}
}

araya::task<void> boot(app& a) {
	a.desired["logger"] = desired_entry{&araya::logger::plugin_descriptor(), {{"name", "araya"}, {"level", "info"}}};
	a.desired["timer"] = desired_entry{&araya::timer::plugin_descriptor(), {}};
	a.desired["session"] = desired_entry{&araya::session::plugin_descriptor(), {}};
	a.desired["persistence"] = desired_entry{&araya::persistence::plugin_descriptor(), {{"root", "araya-sessions"}}};
	a.desired["beacon"] = desired_entry{&araya::console_demo::beacon_descriptor(), {}};
	a.desired["watcher"] = desired_entry{&araya::console_demo::watcher_descriptor(), {}};
	a.desired["console"] = desired_entry{&araya::console_demo::console_descriptor(), {}};

	co_await a.rt->reconcile(make_desired(a));
	co_await a.rt->wait_idle();

	// The live feed: host-owned listeners on the session firehose. They
	// live until the bus dies with the runtime, and print as events land.
	co_await a.rt->run_on_strand([&] {
		auto* bus = a.rt->bus().get();
		bus->add_listener(
			araya::session::created_key,
			[](araya::session::session_created_msg const& m) { out("event: session/created " + m.s->id().value); },
			0);
		bus->add_listener(
			araya::session::disposed_key,
			[](araya::session::session_disposed_msg const& m) { out("event: session/disposed " + m.id.value); },
			0);
		bus->add_listener(
			araya::session::appended_key,
			[](araya::session::session_appended_msg const& m) {
				out("event: session/event " + m.id.value + " seq " + std::to_string(m.event.seq) + " type " +
					m.event.type);
			},
			0);
	});

	out("araya console: " + std::to_string(a.desired.size()) + " components mounted, type help");
}

araya::task<void> run_console(app& a, std::optional<std::string> script_path) {
	try {
		co_await boot(a);

		if (script_path) {
			std::ifstream script(*script_path);
			if (!script) {
				out("error: cannot open script '" + *script_path + "'");
				co_return;
			}
			std::string line;
			while (std::getline(script, line) && !a.quitting) {
				out("> " + line);
				co_await exec_line(a, std::move(line));
			}
			if (!a.quitting)
				co_await exec_line(a, "quit");
		} else {
			boost::asio::posix::stream_descriptor in(a.io, ::dup(STDIN_FILENO));
			boost::asio::streambuf buffer;
			while (!a.quitting) {
				prompt();
				std::size_t consumed = 0;
				try {
					consumed = co_await boost::asio::async_read_until(in, buffer, '\n', boost::asio::use_awaitable);
				} catch (boost::system::system_error const& e) {
					if (e.code() != boost::asio::error::eof && e.code() != boost::asio::error::operation_aborted)
						out(std::string("error: stdin: ") + e.what());
					break;
				}
				// Direct-buffers extraction, not istream: getline advances
				// the streambuf's get pointer, and consume(n) advances it
				// again - which corrupts reads that delivered bytes past
				// the delimiter. Here nothing advances the pointer, so the
				// single consume(n) lands exactly past the line.
				auto data = buffer.data();
				std::string line(boost::asio::buffers_begin(data), boost::asio::buffers_begin(data) + consumed - 1);
				while (!line.empty() && line.back() == '\r')
					line.pop_back();
				buffer.consume(consumed);
				co_await exec_line(a, std::move(line));
			}
		}

		// Retire the whole tree while the io_context still runs, so the
		// runtime destructor (at scope exit, with the context drained)
		// has nothing left to do - the teardown contract in runtime.hpp.
		co_await a.rt->reconcile({});
		co_await a.rt->wait_idle();
		out("bye");
	} catch (std::exception const& e) {
		out(std::string("fatal: ") + e.what());
	}
}

} // namespace

int main(int argc, char** argv) {
	std::optional<std::string> script_path;
	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--script" && i + 1 < argc) {
			script_path = argv[++i];
		} else if (arg == "--help" || arg == "-h") {
			std::cout << "usage: araya_console [--script <file>]\n\n";
			std::cout << g_help << '\n';
			return 0;
		} else {
			std::cerr << "unknown argument: " << arg << "\nusage: araya_console [--script <file>]\n";
			return 2;
		}
	}

	araya::console_demo::init_demo_state(std::make_shared<araya::console_demo::demo_state>());

	app a;
	boost::asio::co_spawn(a.io, run_console(a, std::move(script_path)), boost::asio::detached);
	a.io.run();
	return 0;
}
