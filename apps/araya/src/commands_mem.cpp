#include "commands.hpp"

#include "araya/task.hpp"

#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

// The memory-snapshot command: process RSS/VM fields from /proc/self/status
// (allocator-agnostic, so it stays meaningful under an LD_PRELOAD'd
// jemalloc/tcmalloc) plus the glibc allocator's own arena accounting, and an
// optional malloc_info XML dump for offline fragmentation analysis.
namespace araya::app {
namespace {

// One whitespace-trimmed field value from /proc/self/status.
std::string status_field(std::string_view key) {
	std::ifstream status("/proc/self/status");
	std::string line;
	while (std::getline(status, line)) {
		if (line.rfind(key, 0) != 0)
			continue;
		auto const colon = line.find(':');
		if (colon != std::string::npos)
			return trim(line.substr(colon + 1));
	}
	return "(unavailable)";
}

} // namespace

araya::task<void> cmd_mem(app_context&, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		std::string sub;
		is >> cmd >> sub;

		out("mem: VmRSS " + status_field("VmRSS") + "  VmData " + status_field("VmData") + "  VmPeak " +
			status_field("VmPeak") + "  VmSize " + status_field("VmSize") + "  Threads " + status_field("Threads"));

#if defined(__GLIBC__)
		struct mallinfo2 mi = mallinfo2();
		out("mem: mallinfo2 arena=" + std::to_string(mi.arena) + " hblkhd=" + std::to_string(mi.hblkhd) +
			" uordblks=" + std::to_string(mi.uordblks) + " fordblks=" + std::to_string(mi.fordblks) +
			" keepcost=" + std::to_string(mi.keepcost));
#else
		out("mem: mallinfo2 unavailable (non-glibc)");
#endif

		if (sub == "dump") {
			std::string path;
			std::getline(is, path);
			path = trim(std::move(path));
			if (path.empty()) {
				out("mem: usage: mem [dump <path>]");
				co_return;
			}
#if defined(__GLIBC__)
			FILE* file = std::fopen(path.c_str(), "w");
			if (!file) {
				out("mem: cannot open '" + path + "'");
				co_return;
			}
			int const rc = malloc_info(0, file);
			std::fclose(file);
			out("mem: " + std::string(rc == 0 ? "wrote malloc_info to " : "malloc_info failed for ") + path);
#else
			out("mem: dump unavailable (non-glibc)");
#endif
		}
	} catch (std::exception const& e) {
		out(std::string("mem: ") + e.what());
	}
	co_return;
}

} // namespace araya::app
