#include "app_core.hpp"
#include "config.hpp"
#include "log_setup.hpp"

#include "araya/logger/logger.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The unified araya executable: one binary, first-party everything
// statically linked, dispatching on the first argument. The surfaces
// keep their own entry logic in run_main.cpp and tui_main.cpp.

int run_main(int argc, char** argv, araya::app::app_config config);
int tui_main(std::string session, araya::app::app_config config);

namespace {

void usage() {
	std::cout << "usage: araya [--config <path>] [--log-level <lvl>] [--log-file <path>] [--print-config] <run|tui> "
				 "[arguments]\n\n"
			  << "  run <script>          replay a command script headlessly\n"
			  << "  tui [-s|--session id] the FTXUI terminal UI (requires a terminal; Ctrl+D quits)\n\n"
			  << "  --config <path>       extra config overlay (repeatable; also ARAYA_CONFIG)\n"
			  << "  --log-level <lvl>     error|warn|info|debug (also ARAYA_LOG_LEVEL; default info)\n"
			  << "  --log-file <path>     rotating log file (also ARAYA_LOG_FILE)\n"
			  << "  --print-config        print the resolved configuration and exit\n";
}

} // namespace

int main(int argc, char** argv) {
	// Launcher-level flags are stripped (from any position) before the
	// subcommand arguments are dispatched.
	std::vector<char*> args;
	std::vector<std::string> overlays;
	araya::app::config_cli cli;
	bool print_config = false;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--config" && i + 1 < argc) {
			overlays.emplace_back(argv[++i]);
			continue;
		}
		if (arg == "--log-level" && i + 1 < argc) {
			cli.log_level = argv[++i];
			continue;
		}
		if (arg == "--log-file" && i + 1 < argc) {
			cli.log_file = argv[++i];
			continue;
		}
		if (arg == "--print-config") {
			print_config = true;
			continue;
		}
		args.push_back(argv[i]);
	}

	std::vector<araya::app::config_warning> warnings;
	araya::app::app_config config;
	try {
		config = araya::app::resolve_app_config(overlays, cli, warnings);
	} catch (std::exception const& e) {
		std::cerr << e.what() << '\n';
		return 2;
	}
	if (!araya::logger::parse_level(config.log_level)) {
		std::cerr << "araya: unknown log level '" << config.log_level << "' (error|warn|info|debug)\n";
		return 2;
	}
	if (print_config) {
		try {
			for (auto const& [id, knobs] : config.components)
				araya::app::validate_component(araya::app::real_descriptor, id, knobs, warnings);
		} catch (std::exception const& e) {
			std::cerr << e.what() << '\n';
			return 2;
		}
		for (auto const& warning : warnings)
			std::cerr << "araya: config warning: " << warning.source << ": " << warning.detail << '\n';
		std::cout << araya::app::render_app_config(config, araya::app::real_descriptor);
		return 0;
	}
	for (auto const& warning : warnings)
		std::cerr << "araya: config warning: " << warning.source << ": " << warning.detail << '\n';

	if (args.size() < 2) {
		usage();
		return 1;
	}
	std::string_view subcommand = args[1];
	if (subcommand == "--help" || subcommand == "-h") {
		usage();
		return 0;
	}
	if (subcommand == "run") {
		// Shift the subcommand out of the argument vector.
		return run_main(static_cast<int>(args.size()) - 1, args.data() + 1, std::move(config));
	}
	if (subcommand == "tui") {
		std::string session;
		for (std::size_t i = 2; i < args.size(); ++i) {
			std::string_view arg = args[i];
			if ((arg == "-s" || arg == "--session") && i + 1 < args.size()) {
				session = args[++i];
			} else {
				std::cerr << "usage: araya tui [-s|--session <id>]\n";
				return 2;
			}
		}
		return tui_main(std::move(session), std::move(config));
	}
	std::cerr << "unknown subcommand '" << subcommand << "'\n";
	usage();
	return 1;
}
