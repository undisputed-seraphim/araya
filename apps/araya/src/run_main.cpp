#include "app_core.hpp"

#include "demo_plugins.hpp"
#include "log_setup.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
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

std::string trim(std::string text) {
	auto first = text.find_first_not_of(" \t\r");
	if (first == std::string::npos)
		return {};
	auto last = text.find_last_not_of(" \t\r");
	return text.substr(first, last - first + 1);
}

// The `/source` nesting cap; deeper inclusion is refused so a self-referential
// script cannot recurse forever.
constexpr int max_source_depth = 8;
constexpr long long max_repeat = 1'000'000;

// Runs one line. Pseudo-commands stay here: quit controls the loop, sleep
// paces the script, repeat loops a command, source includes another script.
// A leading '/' is optional on the pseudo-commands, so `/repeat` and `repeat`
// both work. Everything else dispatches on the control strand.
araya::task<void>
exec_line(araya::app::app_context& ctx, bool& quitting, std::string line, std::filesystem::path const& base, int depth);

// Execute every line of `path` (used by `/source`). Relative includes resolve
// against the including file's directory.
araya::task<void>
source_file(araya::app::app_context& ctx, bool& quitting, std::filesystem::path const& path, int depth) {
	if (depth > max_source_depth) {
		out("source: maximum nesting depth (" + std::to_string(max_source_depth) + ") reached");
		co_return;
	}
	std::ifstream file(path);
	if (!file) {
		out("source: cannot open '" + path.string() + "'");
		co_return;
	}
	auto const base = path.parent_path();
	std::string line;
	while (std::getline(file, line) && !quitting) {
		out("> " + line);
		co_await exec_line(ctx, quitting, std::move(line), base, depth);
	}
}

araya::task<void> exec_line(
	araya::app::app_context& ctx,
	bool& quitting,
	std::string line,
	std::filesystem::path const& base,
	int depth) {
	line = trim(std::move(line));
	if (line.empty())
		co_return;

	std::istringstream is(line);
	std::string cmd;
	is >> cmd;
	std::string pseudo = cmd;
	if (!pseudo.empty() && pseudo.front() == '/')
		pseudo.erase(0, 1);

	if (pseudo == "quit") {
		quitting = true;
		out("quit: retiring the tree");
		co_return;
	}
	if (pseudo == "sleep") {
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
	if (pseudo == "repeat") {
		long long count = 0;
		if (!(is >> count) || count < 0 || count > max_repeat) {
			out("repeat: usage: repeat <0..1000000> <command>");
			co_return;
		}
		std::string rest;
		std::getline(is, rest);
		rest = trim(std::move(rest));
		if (rest.empty()) {
			out("repeat: usage: repeat <0..1000000> <command>");
			co_return;
		}
		out("repeat: " + std::to_string(count) + " x " + rest);
		for (long long i = 0; i < count && !quitting; ++i)
			co_await exec_line(ctx, quitting, rest, base, depth);
		co_return;
	}
	if (pseudo == "source") {
		std::string path;
		std::getline(is, path);
		path = trim(std::move(path));
		if (path.empty()) {
			out("source: usage: source <path>");
			co_return;
		}
		std::filesystem::path resolved(path);
		if (resolved.is_relative())
			resolved = base / resolved;
		co_await source_file(ctx, quitting, resolved, depth + 1);
		co_return;
	}

	araya::app::line_sink sink = [](std::string text) { out(std::move(text)); };
	co_await boost::asio::co_spawn(
		ctx.rt->bus()->executor(), araya::app::dispatch(ctx, sink, line), boost::asio::use_awaitable);
}

araya::task<void> run(araya::app::app_context& ctx, std::string script_path) {
	try {
		araya::app::line_sink sink = [](std::string text) { out(std::move(text)); };

		std::ifstream script(script_path);
		if (!script) {
			out("error: cannot open script '" + script_path + "'");
			co_return;
		}
		// `ask_user_question` reads its answer from the next top-level script
		// line (the console answerer). Lines pulled in via /source or /repeat
		// reuse this same top-level stream.
		ctx.answer_input = [&script, &sink](std::string_view prompt) {
			sink(std::string(prompt));
			std::string answer;
			std::getline(script, answer);
			return answer;
		};
		co_await araya::app::boot(ctx, sink);

		auto const base = std::filesystem::path(script_path).parent_path();
		bool quitting = false;
		std::string line;
		while (std::getline(script, line) && !quitting) {
			out("> " + line);
			co_await exec_line(ctx, quitting, std::move(line), base, 0);
		}
		if (!quitting)
			co_await exec_line(ctx, quitting, "quit", base, 0);

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

int run_main(int argc, char** argv, araya::app::app_config config) {
	// argv[0] is "run"; the script path follows. --llm-config overrides the
	// resolved LLM route with a provider config file.
	std::string script;
	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--llm-config" && i + 1 < argc) {
			config.llm_config_file = std::filesystem::absolute(argv[++i]).string();
			config.llm_config_json.reset();
		} else if (arg == "--help" || arg == "-h") {
			std::cout << "usage: araya run <script> [--llm-config <path>]\n\n";
			std::cout << araya::app::help_text() << '\n';
			std::cout << "  sleep <ms>              wait (script mode)\n";
			std::cout << "  repeat <n> <command>    run a command n times (script mode)\n";
			std::cout << "  source <path>           include another script (script mode)\n";
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

	araya::console_demo::init_demo_state(std::make_shared<araya::console_demo::demo_state>());

	// Headless logging: the file sink always, plus a console mirror only when
	// stdout is a terminal, so script output is not interleaved with records
	// (and the ctests, which are not TTYs, capture only the script's output).
	araya::app::log_settings logs;
	logs.file = config.log_file;
	logs.level = config.log_level;
	logs.console = isatty(STDOUT_FILENO) != 0;
	araya::app::install_log_sinks(logs);

	araya::app::app_context ctx;
	ctx.config = std::move(config);
	boost::asio::co_spawn(ctx.io, run(ctx, std::move(script)), boost::asio::detached);
	ctx.io.run();
	araya::app::shutdown_logging();
	return 0;
}
