#include "tui_state.hpp"

#include "araya/fiber_handle.hpp"
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
	ns->session = e.ctx.current ? e.ctx.current->value : "no session";
	ns->cwd_branch = e.ctx.cwd_branch;
	ns->cwd = e.ctx.cwd;
	ns->version = araya::version_string();
	sh.snap.store(std::move(ns), std::memory_order_release);
	if (sh.wake)
		sh.wake();
}

// Commands arrive as strings from the UI and run on the control strand
// (the shared app layer's contract); output lands in the log.
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
			araya::app::line_sink sink = [&e](std::string text) { log_line(e, std::move(text)); };
			try {
				co_await araya::app::dispatch(e.ctx, sink, line);
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
		publish_snapshot(sh, e);
	}
}

araya::task<void> boot_and_run(shared_state& sh, engine_state& e) {
	araya::app::line_sink sink = [&e](std::string text) { log_line(e, std::move(text)); };
	co_await araya::app::boot(e.ctx, sink);
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

void run_engine(shared_state& sh) {
	engine_state e;
	if (auto const* env = std::getenv("ARAYA_LLM_CONFIG"); env && *env)
		e.ctx.llm_config = env;
	boost::asio::co_spawn(e.ctx.io.get_executor(), boot_and_run(sh, e), boost::asio::detached);
	e.ctx.io.run();
}

} // namespace araya::tui
