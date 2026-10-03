#pragma once

#include <string>

// The app's logging setup: the settings shared by every entry point, the
// sink installation that must run before the logger plugin applies, and the
// process-end flush. Kept in the app layer (not the logger plugin) so the
// concrete file path, rotation policy, and the set of pre-created logger
// names stay app conventions.
namespace araya::app {

struct log_settings {
	// The rotating log file, relative to the working directory by default
	// (gitignored as araya-tui.log*). ARAYA_LOG_FILE overrides it.
	std::string file = "araya-tui.log";
	// One of error|warn|info|debug; ARAYA_LOG_LEVEL overrides it. Applied by
	// the logger service, so install_log_sinks does not need to parse it.
	std::string level = "info";
	// Also mirror records to stdout through a quill console sink. Interactive
	// surfaces keep this off so nothing writes over the FTXUI canvas.
	bool console = false;
};

// Applies the ARAYA_LOG_FILE / ARAYA_LOG_LEVEL environment variables over
// `base` (a set, non-empty variable wins).
log_settings resolve_log_settings(log_settings base = {});

// Pre-creates the app's known quill loggers with a rotating file sink (and,
// when `console`, a stdout console sink) so the logger service adopts them by
// name at boot. Call before the desired tree is mounted; the logger plugin's
// apply() adopts existing quill loggers instead of attaching a console sink.
void install_log_sinks(log_settings const& settings);

// Flushes every known logger and stops quill's backend thread. Call once at
// process end, after the engine has retired and stopped producing records.
void shutdown_logging();

} // namespace araya::app
