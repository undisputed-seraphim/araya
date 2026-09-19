#include "demo_plugins.hpp"

#include "araya/fiber_handle.hpp"
#include "araya/logger/logger.hpp"
#include "araya/persistence/persistence.hpp"
#include "araya/runtime.hpp"
#include "araya/session/events.hpp"
#include "araya/session/store.hpp"
#include "araya/timer/timer.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/RotatingFileSink.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The FTXUI terminal UI. The engine (runtime + plugins + command
// execution) runs on a background thread; the UI thread owns the
// FTXUI loop and only ever reads immutable snapshots published by the
// engine - the strand discipline stays entirely on the engine side.

namespace {

using namespace std::chrono_literals;

using araya::logger::logger_key;
using araya::logger::logger_service;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::timer::timer_key;
using araya::timer::timer_service;

// -- the cross-thread contract -------------------------------------------

struct component_row {
	std::string name;
	std::string state;
	std::string error;
};

// An immutable UI snapshot: the engine builds a fresh one and swaps it
// in atomically; the UI thread copies the shared_ptr and renders.
struct snapshot {
	std::vector<component_row> components;
	std::vector<std::string> log;
	std::string status;
};

struct shared_state {
	std::atomic<std::shared_ptr<const snapshot>> snap{std::make_shared<const snapshot>()};
	std::mutex command_mutex;
	std::deque<std::string> commands;
	std::atomic<bool> quit{false};
};

ftxui::ScreenInteractive* g_screen = nullptr;

constexpr std::size_t k_max_log = 1000;

// -- engine side (engine thread only) -------------------------------------

struct desired_entry {
	araya::plugin_descriptor const* descriptor = nullptr;
	araya::plugin_config config;
};

struct engine_state {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	std::map<std::string, desired_entry> desired;
	std::optional<araya::session::session_id> current;
	std::vector<araya::registration> intervals;
	std::vector<component_row> components;
	std::deque<std::string> log;
};

std::shared_ptr<araya::plugin_descriptor> borrow(araya::plugin_descriptor const& d) {
	return std::shared_ptr<araya::plugin_descriptor>(
		const_cast<araya::plugin_descriptor*>(&d), [](araya::plugin_descriptor*) {});
}

std::vector<araya::desired_component> make_desired(engine_state& e) {
	std::vector<araya::desired_component> result;
	result.reserve(e.desired.size());
	for (auto const& [path, entry] : e.desired)
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
	} catch (std::exception const& ex) {
		return ex.what();
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

void log_line(engine_state& e, std::string text) {
	e.log.push_back(std::move(text));
	while (e.log.size() > k_max_log)
		e.log.pop_front();
}

void publish_snapshot(shared_state& sh, engine_state& e) {
	auto ns = std::make_shared<snapshot>();
	ns->components = e.components;
	ns->log.assign(e.log.begin(), e.log.end());
	std::size_t active = 0;
	for (auto const& c : e.components) {
		if (c.state == "active")
			++active;
	}
	ns->status = "components: " + std::to_string(e.components.size()) + " (" + std::to_string(active) + " active)";
	if (e.current)
		ns->status += " | session: " + e.current->value;
	sh.snap.store(std::move(ns), std::memory_order_release);
	if (auto* screen = g_screen)
		screen->PostEvent(ftxui::Event::Custom);
}

// -- commands (run on the control strand) ---------------------------------

boost::json::value user_message(std::string_view id, std::string_view text) {
	return {
		{"id", std::string(id)},
		{"role", "user"},
		{"content", boost::json::array{{{"type", "text"}, {"text", std::string(text)}}}},
	};
}

araya::task<void> run_command(shared_state& sh, engine_state& e, std::string line) {
	auto first = line.find_first_not_of(" \t");
	if (first == std::string::npos)
		co_return;
	auto last = line.find_last_not_of(" \t");
	line = line.substr(first, last - first + 1);

	std::istringstream is(line);
	std::string cmd;
	is >> cmd;

	if (cmd == "help") {
		log_line(e, "commands: ls, session new|append|replace|show|list, timer in|cancel,");
		log_line(e, "          fail <x>|clear, avail wait|ready, log level, quit");
	} else if (cmd == "ls") {
		auto fibers = co_await e.rt->fibers_async();
		for (auto const& f : fibers) {
			log_line(
				e,
				"  " + f.name + " (" + f.descriptor + ") " + state_name(f.state) +
					(f.error ? " error: " + error_text(f.error) : ""));
		}
	} else if (cmd == "session") {
		std::string sub;
		is >> sub;
		if (sub == "new") {
			std::string id;
			is >> id;
			auto root_ctx = e.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			araya::session::session_id sid = id.empty() ? store->mint_id() : araya::session::session_id{std::move(id)};
			(void)store->create(root_ctx, sid, {});
			e.current = sid;
			log_line(e, "session: created " + sid.value);
		} else if (sub == "append") {
			if (!e.current) {
				log_line(e, "session: no current session");
			} else {
				std::string text;
				std::getline(is, text);
				auto t = text.find_first_not_of(' ');
				if (t != std::string::npos)
					text = text.substr(t);
				auto root_ctx = e.rt->root_context();
				auto store = root_ctx.require<session_store>(sessions_key);
				auto s = store->get(*e.current);
				if (!s) {
					log_line(e, "session: current session was disposed");
					e.current.reset();
				} else {
					auto seq = s->append("user/message", user_message("u" + std::to_string(s->log().size()), text));
					log_line(e, "session: appended seq " + std::to_string(seq));
				}
			}
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
				log_line(e, "session: usage: session replace <start> <end> <text>");
			} else if (!e.current) {
				log_line(e, "session: no current session");
			} else {
				auto root_ctx = e.rt->root_context();
				auto store = root_ctx.require<session_store>(sessions_key);
				auto s = store->get(*e.current);
				if (!s) {
					log_line(e, "session: current session was disposed");
					e.current.reset();
				} else {
					boost::json::value replace = {
						{"start_seq", start},
						{"end_seq", end},
						{"message", user_message("r" + std::to_string(s->log().size()), text)},
					};
					auto seq = s->append("surface/replace", std::move(replace));
					log_line(e, "session: replaced [" + std::to_string(start) + ", " + std::to_string(end) + ")");
					(void)seq;
				}
			}
		} else if (sub == "show") {
			if (!e.current) {
				log_line(e, "session: no current session");
			} else {
				auto root_ctx = e.rt->root_context();
				auto store = root_ctx.require<session_store>(sessions_key);
				auto s = store->get(*e.current);
				if (!s) {
					log_line(e, "session: current session was disposed");
					e.current.reset();
				} else {
					for (auto const& m : s->surface().messages())
						log_line(e, "  " + role_name(m.role) + ": " + boost::json::serialize(m.content));
				}
			}
		} else if (sub == "list") {
			auto root_ctx = e.rt->root_context();
			auto store = root_ctx.require<session_store>(sessions_key);
			for (auto const& id : store->list())
				log_line(e, std::string(e.current && e.current->value == id.value ? "* " : "  ") + id.value);
		} else {
			log_line(e, "session: new | append | replace | show | list");
		}
	} else if (cmd == "timer") {
		std::string sub;
		is >> sub;
		if (sub == "in") {
			double seconds = 0.0;
			is >> seconds;
			std::string text;
			std::getline(is, text);
			auto t = text.find_first_not_of(' ');
			if (t != std::string::npos)
				text = text.substr(t);
			if (!is || text.empty()) {
				log_line(e, "timer: usage: timer in <seconds> <text>");
			} else {
				auto root_ctx = e.rt->root_context();
				auto timer = root_ctx.require<timer_service>(timer_key);
				auto delay =
					std::chrono::duration_cast<timer_service::duration>(std::chrono::duration<double>(seconds));
				(void)timer->timeout(root_ctx, delay, [&sh, &e, text] {
					log_line(e, "timer: " + text);
					publish_snapshot(sh, e);
				});
				log_line(e, "timer: armed");
			}
		} else if (sub == "cancel") {
			for (auto& reg : e.intervals)
				reg.release();
			e.intervals.clear();
			log_line(e, "timer: cancelled");
		} else {
			log_line(e, "timer: in | cancel");
		}
	} else if (cmd == "fail") {
		std::string name;
		is >> name;
		if (name == "clear") {
			for (auto const& key : {"logger", "timer", "session"}) {
				auto it = e.desired.find(key);
				if (it != e.desired.end()) {
					it->second.descriptor = real_descriptor(key);
					it->second.config = default_config(key);
				}
			}
			co_await e.rt->reconcile(make_desired(e));
			co_await e.rt->wait_idle();
			log_line(e, "fail: providers restored");
		} else if (name != "logger" && name != "timer" && name != "session") {
			log_line(e, "fail targets: logger, timer, session (or 'clear')");
		} else {
			auto it = e.desired.find(name);
			if (it == e.desired.end()) {
				log_line(e, name + ": not loaded");
			} else {
				it->second.descriptor = &araya::console_demo::bomb_descriptor_for(name);
				it->second.config = {{"what", name}};
				co_await e.rt->reconcile(make_desired(e));
				co_await e.rt->wait_idle();
				log_line(e, "fail: " + name + " swapped for a crashing provider");
			}
		}
	} else if (cmd == "avail") {
		std::string action;
		is >> action;
		if (!e.desired.contains("beacon")) {
			log_line(e, "beacon: not loaded");
		} else if (action == "wait") {
			araya::console_demo::state().beacon_ready.store(false, std::memory_order_release);
			auto entry = e.desired.at("beacon");
			e.desired.erase("beacon");
			co_await e.rt->reconcile(make_desired(e));
			co_await e.rt->wait_idle();
			e.desired["beacon"] = entry;
			co_await e.rt->reconcile(make_desired(e));
			co_await e.rt->wait_idle();
			log_line(e, "avail: beacon re-published unavailable (watcher parks)");
		} else if (action == "ready") {
			araya::console_demo::state().beacon_ready.store(true, std::memory_order_release);
			e.rt->bus()->dispatch(araya::console_demo::beacon_ready_key, std::string("go"));
			co_await e.rt->wait_idle();
			log_line(e, "avail: beacon promoted (watcher activates)");
		} else {
			log_line(e, "avail: wait | ready");
		}
	} else if (cmd == "log") {
		std::string level;
		is >> level;
		auto root_ctx = e.rt->root_context();
		auto logger = root_ctx.require<logger_service>(logger_key);
		if (level == "debug")
			logger->set_level(araya::logger::log_level::debug);
		else if (level == "warn")
			logger->set_level(araya::logger::log_level::warn);
		else if (level == "error")
			logger->set_level(araya::logger::log_level::error);
		else if (level == "info")
			logger->set_level(araya::logger::log_level::info);
		else
			log_line(e, "log level: debug | info | warn | error");
	} else {
		log_line(e, "unknown command '" + cmd + "' (try help)");
	}
}

// -- engine loops ---------------------------------------------------------

araya::task<void> command_loop(shared_state& sh, engine_state& e) {
	// Bound to the control strand: every resume lands back on it, so
	// run_command's store/service calls keep the strand contract.
	boost::asio::steady_timer timer(e.rt->bus()->executor());
	while (!sh.quit.load(std::memory_order_relaxed)) {
		std::string line;
		{
			std::lock_guard lock(sh.command_mutex);
			if (!sh.commands.empty()) {
				line = std::move(sh.commands.front());
				sh.commands.pop_front();
			}
		}
		if (!line.empty()) {
			try {
				co_await run_command(sh, e, std::move(line));
			} catch (std::exception const& ex) {
				log_line(e, std::string("error: ") + ex.what());
			}
			publish_snapshot(sh, e);
		}
		timer.expires_after(50ms);
		co_await timer.async_wait(boost::asio::use_awaitable);
	}
}

araya::task<void> refresh_loop(shared_state& sh, engine_state& e) {
	boost::asio::steady_timer timer(e.io.get_executor());
	while (!sh.quit.load(std::memory_order_relaxed)) {
		timer.expires_after(400ms);
		co_await timer.async_wait(boost::asio::use_awaitable);
		auto fibers = co_await e.rt->fibers_async();
		e.components.clear();
		for (auto const& f : fibers) {
			component_row row;
			row.name = f.name;
			row.state = state_name(f.state);
			row.error = f.error ? error_text(f.error) : std::string{};
			e.components.push_back(std::move(row));
		}
		publish_snapshot(sh, e);
	}
}

araya::task<void> boot_and_run(shared_state& sh, engine_state& e) {
	e.desired["logger"] = desired_entry{&araya::logger::plugin_descriptor(), {{"name", "araya"}, {"level", "info"}}};
	e.desired["timer"] = desired_entry{&araya::timer::plugin_descriptor(), {}};
	e.desired["session"] = desired_entry{&araya::session::plugin_descriptor(), {}};
	e.desired["persistence"] = desired_entry{&araya::persistence::plugin_descriptor(), {{"root", "araya-sessions"}}};
	e.desired["beacon"] = desired_entry{&araya::console_demo::beacon_descriptor(), {}};
	e.desired["watcher"] = desired_entry{&araya::console_demo::watcher_descriptor(), {}};
	e.desired["console"] = desired_entry{&araya::console_demo::console_descriptor(), {}};

	co_await e.rt->reconcile(make_desired(e));
	co_await e.rt->wait_idle();

	// Host-owned feed listeners: session events land in the log pane.
	co_await e.rt->run_on_strand([&] {
		auto* bus = e.rt->bus().get();
		bus->add_listener(
			araya::session::created_key,
			[&e](araya::session::session_created_msg const& m) {
				log_line(e, "event: session/created " + m.s->id().value);
			},
			0);
		bus->add_listener(
			araya::session::appended_key,
			[&e](araya::session::session_appended_msg const& m) {
				log_line(
					e,
					"event: session/event " + m.id.value + " seq " + std::to_string(m.event.seq) + " type " +
						m.event.type);
			},
			0);
		bus->add_listener(
			araya::session::disposed_key,
			[&e](araya::session::session_disposed_msg const& m) {
				log_line(e, "event: session/disposed " + m.id.value);
			},
			0);
	});

	log_line(e, "araya tui: components mounted, type help (Ctrl+C quits)");
	publish_snapshot(sh, e);

	boost::asio::co_spawn(e.rt->bus()->executor(), command_loop(sh, e), boost::asio::detached);
	boost::asio::co_spawn(e.io.get_executor(), refresh_loop(sh, e), boost::asio::detached);

	// Wait for the UI to signal quit, then retire the whole tree while
	// the context still runs - the runtime destructor (at engine_state
	// teardown, after io.run() drains) then has nothing left to do.
	boost::asio::steady_timer quit_watch(e.io.get_executor());
	while (!sh.quit.load(std::memory_order_relaxed)) {
		quit_watch.expires_after(100ms);
		co_await quit_watch.async_wait(boost::asio::use_awaitable);
	}
	co_await e.rt->reconcile({});
	co_await e.rt->wait_idle();
}

} // namespace

int main() {
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		std::cerr << "araya_tui needs a terminal (interactive only)\n";
		return 2;
	}

	// The logger service adopts pre-created quill loggers by name, so
	// pre-create the known ones with a file sink - nothing writes over
	// the FTXUI canvas.
	quill::Backend::start();
	for (auto const* name : {"araya", "console", "watcher", "main", "timer"}) {
		quill::RotatingFileSinkConfig sink_config;
		(void)quill::Frontend::create_or_get_logger(
			name, std::make_shared<quill::RotatingFileSink>(std::filesystem::path{"araya-tui.log"}, sink_config));
	}

	araya::console_demo::init_demo_state(std::make_shared<araya::console_demo::demo_state>());

	shared_state sh;
	std::thread engine_thread([&sh] {
		engine_state e;
		boost::asio::co_spawn(e.io.get_executor(), boot_and_run(sh, e), boost::asio::detached);
		e.io.run();
	});

	auto screen = ftxui::ScreenInteractive::Fullscreen();
	g_screen = &screen;

	std::string input_buffer;
	ftxui::InputOption input_options;
	input_options.placeholder = "command (help)";
	ftxui::Component input = ftxui::Input(&input_buffer, input_options);
	input |= ftxui::CatchEvent([&](ftxui::Event event) {
		if (event != ftxui::Event::Return)
			return false;
		auto cmd = input_buffer;
		input_buffer.clear();
		if (cmd == "quit") {
			screen.Exit();
			return true;
		}
		if (!cmd.empty()) {
			std::lock_guard lock(sh.command_mutex);
			sh.commands.push_back(std::move(cmd));
		}
		screen.PostEvent(ftxui::Event::Custom);
		return true;
	});

	auto components_view = ftxui::Renderer([&] {
		auto snap = sh.snap.load(std::memory_order_acquire);
		ftxui::Elements rows;
		rows.reserve(snap->components.size());
		for (auto const& c : snap->components) {
			auto color = ftxui::Color::Green;
			if (c.state == "inactive")
				color = ftxui::Color::Red;
			else if (c.state == "loading" || c.state == "unloading")
				color = ftxui::Color::Yellow;
			rows.push_back(ftxui::hbox({
				ftxui::text(" " + c.name),
				ftxui::filler(),
				ftxui::text(c.state) | ftxui::color(color),
				c.error.empty() ? ftxui::text("") : ftxui::text("  " + c.error) | ftxui::color(ftxui::Color::Red),
			}));
		}
		if (rows.empty())
			rows.push_back(ftxui::text(" (booting...)"));
		return ftxui::vbox(rows) | ftxui::border;
	});

	auto log_view = ftxui::Renderer([&] {
		auto snap = sh.snap.load(std::memory_order_acquire);
		ftxui::Elements lines;
		std::size_t begin = snap->log.size() > 40 ? snap->log.size() - 40 : 0;
		for (std::size_t i = begin; i < snap->log.size(); ++i)
			lines.push_back(ftxui::text(snap->log[i]));
		return ftxui::vbox(lines) | ftxui::border;
	});

	auto status_view = ftxui::Renderer([&] {
		auto snap = sh.snap.load(std::memory_order_acquire);
		return ftxui::text(snap->status.empty() ? "araya tui" : snap->status) | ftxui::dim;
	});

	auto top = ftxui::Renderer([&] {
		return ftxui::hbox({
				   components_view->Render() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 44),
				   log_view->Render() | ftxui::flex,
			   }) |
			   ftxui::flex;
	});

	auto container = ftxui::Container::Vertical({top, input});
	auto root = ftxui::Renderer(container, [&] {
		return ftxui::vbox({
			container->Render() | ftxui::flex,
			ftxui::separator(),
			status_view->Render(),
		});
	});

	// The container starts focused on its first child (the components
	// pane), which would swallow every keystroke: put the focus on the
	// command input explicitly.
	input->TakeFocus();
	screen.Loop(root);

	// Clean teardown: stop the engine, let it retire the tree on the
	// strand, and join before the process exits.
	sh.quit.store(true, std::memory_order_release);
	engine_thread.join();
	quill::Backend::stop();
	g_screen = nullptr;
	return 0;
}
