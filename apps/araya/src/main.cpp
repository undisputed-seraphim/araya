#include <iostream>
#include <string>
#include <string_view>
#include <utility>

// The unified araya executable: one binary, first-party everything
// statically linked, dispatching on the first argument. The surfaces
// keep their own entry logic in run_main.cpp and tui_main.cpp.

int run_main(int argc, char** argv);
int tui_main(std::string session);

namespace {

void usage() {
	std::cout << "usage: araya <run|tui> [arguments]\n\n"
			  << "  run <script>          replay a command script headlessly\n"
			  << "  tui [-s|--session id] the FTXUI terminal UI (requires a terminal; Ctrl+D quits)\n";
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		usage();
		return 1;
	}
	std::string_view subcommand = argv[1];
	if (subcommand == "--help" || subcommand == "-h") {
		usage();
		return 0;
	}
	if (subcommand == "run") {
		// Shift the subcommand out of the argument vector.
		return run_main(argc - 1, argv + 1);
	}
	if (subcommand == "tui") {
		std::string session;
		for (int i = 2; i < argc; ++i) {
			std::string_view arg = argv[i];
			if ((arg == "-s" || arg == "--session") && i + 1 < argc) {
				session = argv[++i];
			} else {
				std::cerr << "usage: araya tui [-s|--session <id>]\n";
				return 2;
			}
		}
		return tui_main(std::move(session));
	}
	std::cerr << "unknown subcommand '" << subcommand << "'\n";
	usage();
	return 1;
}
