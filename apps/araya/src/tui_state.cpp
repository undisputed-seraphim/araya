#include "tui_state.hpp"

#include "input_route.hpp"

#include "araya/fiber_handle.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/session/store.hpp"
#include "araya/version.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <utility>

namespace araya::tui {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t k_max_log = 1000;

void log_line(engine_state& e, std::string text) {
	e.log.push_back(std::move(text));
	while (e.log.size() > k_max_log)
		e.log.pop_front();
}

void publish_snapshot(shared_state& sh, engine_state& e) {
	auto ns = std::make_shared<snapshot>();
	ns->components = e.components;
	ns->log.assign(e.log.begin(), e.log.end());
	ns->messages = e.messages;
	ns->title = e.title;
	ns->started = e.started;
	ns->session = e.ctx.current ? e.ctx.current->value : "no session";
	ns->cwd_branch = e.ctx.cwd_branch;
	ns->cwd = e.ctx.cwd;
	ns->version = araya::version_string();
	sh.snap.store(std::move(ns), std::memory_order_release);
	if (sh.wake)
		sh.wake();
}

// Rebuilds the conversation rows from the current session's folded
// surface. Must run on the control strand: it touches the session store.
void fold_messages(engine_state& e) {
	e.messages.clear();
	if (!e.ctx.current) {
		e.title = "no session";
		return;
	}
	auto root_ctx = e.ctx.rt->root_context();
	auto store_lease = root_ctx.find<araya::session::session_store>(araya::session::sessions_key);
	if (!store_lease) {
		e.title = e.ctx.current->value;
		return;
	}
	auto session = store_lease->shared()->get(*e.ctx.current);
	if (!session) {
		e.title = e.ctx.current->value;
		return;
	}
	std::string first_user;
	for (auto const& message : session->surface().messages()) {
		feed_message row;
		row.role = araya::app::role_name(message.role);
		row.text = araya::llm_bridge::message_text(message);
		if (first_user.empty() && row.role == "user")
			first_user = row.text;
		e.messages.push_back(std::move(row));
	}
	e.title = araya::app::session_title(first_user, e.ctx.current->value);
}

// Commands arrive as strings from the UI and run on the control strand
// (the shared app layer's contract); output lands in the log. Plain text
// routes to the local `say` command so it shows up in the conversation.
araya::task<void> command_loop(shared_state& sh, engine_state& e) {
	boost::asio::steady_timer timer(e.ctx.rt->bus()->executor());
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
			// Any first submit leaves the entry phase.
			if (!e.started) {
				e.started = true;
				sh.started_ui.store(true, std::memory_order_relaxed);
			}
			auto kind =
				araya::app::classify_input(line, [](std::string_view text) { return araya::app::is_command(text); });
			if (kind == araya::app::input_kind::message)
				line = "say " + line;
			araya::app::line_sink sink = [&e](std::string text) { log_line(e, std::move(text)); };
			try {
				co_await araya::app::dispatch(e.ctx, sink, line);
			} catch (std::exception const& ex) {
				log_line(e, std::string("error: ") + ex.what());
			}
			// The strand is ours here: fold the conversation directly.
			fold_messages(e);
			publish_snapshot(sh, e);
		}
		timer.expires_after(50ms);
		co_await timer.async_wait(boost::asio::use_awaitable);
	}
}

araya::task<void> refresh_loop(shared_state& sh, engine_state& e) {
	boost::asio::steady_timer timer(e.ctx.io.get_executor());
	while (!sh.quit.load(std::memory_order_relaxed)) {
		timer.expires_after(400ms);
		co_await timer.async_wait(boost::asio::use_awaitable);
		auto fibers = co_await e.ctx.rt->fibers_async();
		e.components.clear();
		for (auto const& f : fibers) {
			component_row row;
			row.name = f.name;
			row.state = araya::app::state_name(f.state);
			row.error = f.error ? araya::app::error_text(f.error) : std::string{};
			e.components.push_back(std::move(row));
		}
		co_await e.ctx.rt->run_on_strand([&e] { fold_messages(e); });
		publish_snapshot(sh, e);
	}
}

araya::task<void> boot_and_run(shared_state& sh, engine_state& e, std::string resume_id) {
	araya::app::line_sink sink = [&e](std::string text) { log_line(e, std::move(text)); };
	co_await araya::app::boot(e.ctx, sink);

	// Resume: restore the named session from disk (the same path the
	// `session load` command uses) and open straight into the session
	// phase. An unknown id just logs and leaves the entry screen up.
	if (!resume_id.empty()) {
		co_await boost::asio::co_spawn(
			e.ctx.rt->bus()->executor(),
			araya::app::dispatch(e.ctx, sink, "session load " + resume_id),
			boost::asio::use_awaitable);
		if (e.ctx.current && e.ctx.current->value == resume_id)
			e.started = true;
		co_await e.ctx.rt->run_on_strand([&e] { fold_messages(e); });
	}

	log_line(e, "araya tui: components mounted, type help (Ctrl+D quits)");
	publish_snapshot(sh, e);

	boost::asio::co_spawn(e.ctx.rt->bus()->executor(), command_loop(sh, e), boost::asio::detached);
	boost::asio::co_spawn(e.ctx.io.get_executor(), refresh_loop(sh, e), boost::asio::detached);

	// Wait for the UI to signal quit, then retire the whole tree while
	// the context still runs - the runtime destructor (at engine_state
	// teardown, after io.run() drains) then has nothing left to do.
	boost::asio::steady_timer quit_watch(e.ctx.io.get_executor());
	while (!sh.quit.load(std::memory_order_relaxed)) {
		quit_watch.expires_after(100ms);
		co_await quit_watch.async_wait(boost::asio::use_awaitable);
	}
	co_await e.ctx.rt->reconcile({});
	co_await e.ctx.rt->wait_idle();
}

} // namespace

void run_engine(shared_state& sh, std::string resume_id) {
	engine_state e;
	if (auto const* env = std::getenv("ARAYA_LLM_CONFIG"); env && *env)
		e.ctx.llm_config = env;
	boost::asio::co_spawn(e.ctx.io.get_executor(), boot_and_run(sh, e, std::move(resume_id)), boost::asio::detached);
	e.ctx.io.run();
}

} // namespace araya::tui
