#pragma once

#include "araya/fiber_handle.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/session_types.hpp"
#include "araya/task.hpp"
#include "input_route.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json/value.hpp>

#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The shared application layer behind every surface (console, TUI, the
// script runner): the engine-side state, the desired tree, the command
// implementations, and the boot sequence. Zero UI dependencies.
//
// Command contract: handlers are coroutines that assume the control
// strand - the surface's dispatcher co_spawns dispatch() there. Output
// flows through a line_sink (copyable, so deferred callbacks like
// timer fires and the session feed can capture it).
namespace araya::app {

using line_sink = std::function<void(std::string)>;

struct desired_entry {
	araya::plugin_descriptor const* descriptor = nullptr;
	araya::plugin_config config;
};

// The app's engine-side state: the io_context, the runtime, the desired
// tree, and the surface-owned handles. Created in place (the io_context
// is not movable).
struct app_context {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	std::map<std::string, desired_entry> desired;
	std::optional<araya::session::session_id> current;
	std::vector<araya::registration> intervals;
	std::string cwd;
	std::string cwd_branch;
	// Path to the llm-openai config JSON (the --llm-config flag or the
	// ARAYA_LLM_CONFIG environment variable); empty means no adapter.
	std::string llm_config;
};

// A no-op-deleter view over a static descriptor: the same pattern the
// engine tests use. First-party and demo descriptors are static storage.
std::shared_ptr<araya::plugin_descriptor> borrow(araya::plugin_descriptor const& d);

std::vector<araya::desired_component> make_desired(app_context& ctx);

araya::plugin_descriptor const* real_descriptor(std::string_view name);
araya::plugin_config default_config(std::string_view name);

// The palette-facing command metadata (the command table plus help and
// quit): name, usage (which marks arg-taking commands), and summary.
std::span<araya::app::command_info const> command_list();

char const* state_name(araya::fiber_state s);
std::string error_text(std::exception_ptr ep);
std::string_view role_name(araya::session::message_role role);

// The working directory with $HOME collapsed to ~, plus the short git
// branch when the directory is a repository (one subprocess at boot).
std::string cwd_branch_line(std::string const& cwd);

// Mounts the desired tree, records the working directory, and routes
// the session firehose into the sink. Call on the app's io executor;
// the runtime's own teardown contract applies afterwards.
araya::task<void> boot(app_context& ctx, line_sink const& out);

// Parses one command line and runs it on the control strand. The
// pseudo-commands quit/sleep are surface concerns and live outside.
araya::task<void> dispatch(app_context& ctx, line_sink const& out, std::string_view line);

// The help text, generated from the command table.
std::string help_text();

} // namespace araya::app
