#include <iostream>
#include <string_view>

// The unified araya executable: one binary, first-party everything
// statically linked, dispatching on the first argument. The surfaces
// keep their own entry logic in run_main.cpp and tui_main.cpp.

int run_main(int argc, char** argv);
int tui_main();

namespace {

void usage() {
	std::cout << "usage: araya <run|tui> [arguments]\n\n"
			  << "  run <script>  replay a command script headlessly\n"
			  << "  tui           the FTXUI terminal UI (requires a terminal; Ctrl+D quits)\n";
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
		if (argc != 2) {
			std::cerr << "araya tui takes no arguments\n";
			return 2;
		}
		return tui_main();
	}
	std::cerr << "unknown subcommand '" << subcommand << "'\n";
	usage();
	return 1;
}
