#include "log_setup.hpp"

#include "araya/logger/logger.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The unified araya executable: one binary, first-party everything
// statically linked, dispatching on the first argument. The surfaces
// keep their own entry logic in run_main.cpp and tui_main.cpp.

int run_main(int argc, char** argv, araya::app::log_settings logs);
int tui_main(std::string session, araya::app::log_settings logs);

namespace {

void usage() {
	std::cout << "usage: araya [--log-level <lvl>] [--log-file <path>] <run|tui> [arguments]\n\n"
			  << "  run <script>          replay a command script headlessly\n"
			  << "  tui [-s|--session id] the FTXUI terminal UI (requires a terminal; Ctrl+D quits)\n\n"
			  << "  --log-level <lvl>     error|warn|info|debug (also ARAYA_LOG_LEVEL; default info)\n"
			  << "  --log-file <path>     rotating log file (also ARAYA_LOG_FILE; default araya-tui.log)\n";
}

} // namespace

int main(int argc, char** argv) {
	// The global logging flags are stripped (from any position) before the
	// subcommand arguments are dispatched.
	std::vector<char*> args;
	araya::app::log_settings logs;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--log-level" && i + 1 < argc) {
			logs.level = argv[++i];
			continue;
		}
		if (arg == "--log-file" && i + 1 < argc) {
			logs.file = argv[++i];
			continue;
		}
		args.push_back(argv[i]);
	}
	if (!araya::logger::parse_level(logs.level)) {
		std::cerr << "unknown --log-level '" << logs.level << "' (error|warn|info|debug)\n";
		return 2;
	}
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
		return run_main(static_cast<int>(args.size()) - 1, args.data() + 1, std::move(logs));
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
		return tui_main(std::move(session), std::move(logs));
	}
	std::cerr << "unknown subcommand '" << subcommand << "'\n";
	usage();
	return 1;
}
