#include "app_core.hpp"

#include "demo_plugins.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

// The script runner: replays a command script through the shared app
// layer with a stdout sink. This is the headless surface the ctests
// drive (the interactive console retired in favor of the TUI).

namespace {

void out(std::string_view text) { std::cout << text << '\n' << std::flush; }

// Runs one line. Pseudo-commands stay here: quit controls the loop,
// sleep paces the script. Everything else dispatches on the control
// strand.
araya::task<void> exec_line(araya::app::app_context& ctx, bool& quitting, std::string line) {
	auto first = line.find_first_not_of(" \t");
	if (first == std::string::npos)
		co_return;
	auto last = line.find_last_not_of(" \t");
	line = line.substr(first, last - first + 1);

	std::istringstream is(line);
	std::string cmd;
	is >> cmd;

	if (cmd == "quit") {
		quitting = true;
		out("quit: retiring the tree");
		co_return;
	}
	if (cmd == "sleep") {
		std::int64_t ms = 0;
		is >> ms;
		if (!is || ms < 0) {
			out("sleep: usage: sleep <milliseconds>");
		} else {
			boost::asio::steady_timer timer(ctx.io, std::chrono::milliseconds(ms));
			co_await timer.async_wait(boost::asio::use_awaitable);
		}
		co_return;
	}

	araya::app::line_sink sink = [](std::string text) { out(std::move(text)); };
	co_await boost::asio::co_spawn(
		ctx.rt->bus()->executor(), araya::app::dispatch(ctx, sink, line), boost::asio::use_awaitable);
}

araya::task<void> run(araya::app::app_context& ctx, std::string script_path) {
	try {
		araya::app::line_sink sink = [](std::string text) { out(std::move(text)); };
		co_await araya::app::boot(ctx, sink);

		std::ifstream script(script_path);
		if (!script) {
			out("error: cannot open script '" + script_path + "'");
			co_return;
		}
		bool quitting = false;
		std::string line;
		while (std::getline(script, line) && !quitting) {
			out("> " + line);
			co_await exec_line(ctx, quitting, std::move(line));
		}
		if (!quitting)
			co_await exec_line(ctx, quitting, "quit");

		// Retire the whole tree while the io_context still runs, so the
		// runtime destructor (at scope exit, with the context drained)
		// has nothing left to do - the teardown contract in runtime.hpp.
		co_await ctx.rt->reconcile({});
		co_await ctx.rt->wait_idle();
		out("bye");
	} catch (std::exception const& e) {
		out(std::string("fatal: ") + e.what());
	}
}

} // namespace

int run_main(int argc, char** argv) {
	// argv[0] is "run"; the script path follows (--llm-config <path>
	// selects the llm-openai config; ARAYA_LLM_CONFIG is the fallback).
	std::string script;
	std::string llm_config;
	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--llm-config" && i + 1 < argc) {
			llm_config = argv[++i];
		} else if (arg == "--help" || arg == "-h") {
			std::cout << "usage: araya run <script> [--llm-config <path>]\n\n";
			std::cout << araya::app::help_text() << '\n';
			std::cout << "  sleep <ms>              wait (script mode)\n";
			std::cout << "  quit                    retire everything and exit\n";
			return 0;
		} else if (script.empty()) {
			script = arg;
		}
	}
	if (script.empty()) {
		std::cerr << "usage: araya run <script> [--llm-config <path>]\n";
		return 2;
	}
	if (llm_config.empty()) {
		if (auto const* env = std::getenv("ARAYA_LLM_CONFIG"); env && *env)
			llm_config = env;
	}

	araya::console_demo::init_demo_state(std::make_shared<araya::console_demo::demo_state>());

	araya::app::app_context ctx;
	ctx.llm_config = std::move(llm_config);
	boost::asio::co_spawn(ctx.io, run(ctx, std::move(script)), boost::asio::detached);
	ctx.io.run();
	return 0;
}
