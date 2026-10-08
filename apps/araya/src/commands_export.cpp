#include "commands.hpp"

#include "araya/session-log-export/export.hpp"
#include "araya/session/store.hpp"

#include <cstddef>
#include <exception>
#include <fstream>
#include <ios>
#include <string>

// The session-log download: build a ZIP of the current session (plus its
// subagent descendants and referenced images) and write it to disk. A
// distinct output path may be given; otherwise the default download name is
// used in the working directory.
namespace araya::app {
namespace {

std::string argument(std::string const& line) {
	auto const space = line.find_first_of(" \t");
	if (space == std::string::npos)
		return {};
	return trim(line.substr(space + 1));
}

} // namespace

araya::task<void> cmd_export(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		if (!ctx.current) {
			out("export: no current session");
			co_return;
		}
		auto store =
			ctx.rt->root_context().require<araya::session::session_store>(araya::session::sessions_key).shared();
		co_await store->flush(*ctx.current);
		auto service =
			ctx.rt->root_context()
				.require<araya::session_export::session_export_service>(araya::session_export::session_export_key)
				.shared();
		auto const bytes = service->build_zip(*ctx.current, true);
		std::string path = argument(line);
		if (path.empty())
			path = araya::session_export::session_export_filename(*ctx.current);
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file) {
			out("export: cannot open '" + path + "'");
			co_return;
		}
		file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		if (!file) {
			out("export: write failed for '" + path + "'");
			co_return;
		}
		out("exported " + ctx.current->value + " to " + path + " (" + std::to_string(bytes.size()) + " bytes)");
	} catch (std::exception const& error) {
		out(std::string("export failed: ") + error.what());
	}
}

} // namespace araya::app
