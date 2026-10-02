#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"

#include <boost/system/error_code.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The process-confinement seam: confine a child command's file effects to an
// allow-list under a per-call policy, using the Linux Landlock syscall family
// applied IN PROCESS between fork and execve (no helper binary). This is a
// trimmed port of the harness's `@deepseek-ai/dsh-sandbox` service: the mode
// vocabulary, the per-call policy, the enforcement/full-vs-partial fact, and
// the denial-dialect classification. Backends for other platforms, and
// escalation (wider-mode retry via approval), are out of scope here.
//
// Landlock is an allow-list: everything not granted is denied. The seam fails
// closed - a confining mode with no usable backend never runs the command
// unconfined.
namespace araya::sandbox {

// The file-effect mode. `read-only` permits only required sinks (notably
// /dev/null); `workspace-write` also permits the workspace and /tmp;
// `danger-full-access` bypasses confinement entirely.
enum class sandbox_mode : std::uint8_t {
	read_only,
	workspace_write,
	danger_full_access,
};

// The spelled mode, matching the harness vocabulary exactly.
std::string_view mode_name(sandbox_mode mode) noexcept;
std::optional<sandbox_mode> parse_mode(std::string_view name) noexcept;

// How completely the active backend enforces the promised file effects.
// `partial` means the running kernel's Landlock ABI cannot govern every
// effect the mode promises (an older ABI); consumers requiring an absolute
// boundary must not treat it as `full`.
enum class enforcement : std::uint8_t {
	full,
	partial,
};

// The complete file-effect policy resolved for one call. The root is carried
// even under modes that do not consume it so policy can be resolved once
// before the enforcement path is chosen.
struct policy {
	sandbox_mode mode = sandbox_mode::workspace_write;
	std::string workspace_root;
};

namespace detail {

// Apply a Landlock ruleset to the calling process and return a non-zero error
// on failure. Intended to run in the forked child before execve: `read_only`
// paths grant read+execute, `read_write` paths grant full access, and all
// other filesystem access is denied. Defined per platform in the plugin.
boost::system::error_code
apply_landlock(std::vector<std::string> const& read_only, std::vector<std::string> const& read_write) noexcept;

} // namespace detail

// The in-process confinement program, passed to Boost.Process v2 as a spawn
// initializer. Its `on_exec_setup` hook runs in the child after fork and
// before execve; the ruleset is inherited across execve, so the command (and
// every process it spawns) runs confined. An empty program is inert.
struct confinement {
	std::string mode;
	std::vector<std::string> read_only;
	std::vector<std::string> read_write;

	template <class Launcher, class Path>
	boost::system::error_code on_exec_setup(Launcher&, Path const&, char const* const*) const {
		return detail::apply_landlock(read_only, read_write);
	}
};

// One resolved confinement: whether it applies, the enforcement fact, the
// program to install, and the backend's denial dialect (the case-insensitive
// stderr substring a file-effect denial produces).
struct confined {
	bool active = false;
	enforcement level = enforcement::full;
	std::string mode;
	std::string denial_signature;
	confinement init;
};

// Thrown (or reported) when a confining mode is requested but no backend is
// usable. The tool layer surfaces the `SANDBOX_UNAVAILABLE` message rather
// than running the command unconfined.
struct sandbox_unavailable : std::runtime_error {
	explicit sandbox_unavailable(std::string message)
		: std::runtime_error(std::move(message)) {}
};

// The provider: probes the host once and builds confinement programs.
class sandbox_provider {
public:
	virtual ~sandbox_provider() = default;

	// Whether this host can confine at all (a runtime probe, cached).
	virtual bool available() const noexcept = 0;

	// Why confinement is unusable, for the fail-closed diagnostic. Reserved
	// API: the shell surfaces the thrown sandbox_unavailable message instead.
	[[maybe_unused]] virtual std::string const& unavailable_reason() const noexcept = 0;

	// Build the confinement for one call. `danger-full-access` returns an
	// inactive result; a confining mode with no usable backend throws
	// `sandbox_unavailable`.
	virtual confined confine(policy const& p) const = 0;
};

inline constexpr araya::service_key<sandbox_provider> sandbox_key{"sandbox", 1};

// Whether a settled run's stderr matches a denial dialect (case-insensitive
// substring), and the exit was non-zero. A denial means confinement worked
// and blocked the command.
bool matches_denial(int exit_code, std::string_view stderr_text, std::string_view signature);

// The model-facing denial marker both enforcement families teach.
std::string denial_marker(std::string_view mode);

// The plugin descriptor: no dependencies, provides {"sandbox",1}. The
// backend always compiles; when Landlock is unavailable it reports unusable
// and fails closed.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::sandbox
