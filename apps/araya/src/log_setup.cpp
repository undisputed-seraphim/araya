#include "log_setup.hpp"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/RotatingFileSink.h>
#include <quill/sinks/Sink.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::app {
namespace {

// The logger names the app pre-creates; the logger service adopts an
// existing quill logger by name, so anything logging under one of these
// lands in the shared file sink.
constexpr std::string_view kLoggerNames[] = {"araya", "console", "watcher", "main", "timer"};

// Rotate before the file grows unbounded: 5 MiB per file, keeping three
// backups (araya-tui.log.1 .. .3). quill's defaults disable both.
constexpr std::size_t kMaxFileSize = 5u * 1024u * 1024u;
constexpr std::uint32_t kMaxBackupFiles = 3;

} // namespace

log_settings resolve_log_settings(log_settings base) {
	if (auto const* file = std::getenv("ARAYA_LOG_FILE"); file && *file)
		base.file = file;
	if (auto const* level = std::getenv("ARAYA_LOG_LEVEL"); level && *level)
		base.level = level;
	return base;
}

void install_log_sinks(log_settings const& settings) {
	if (!quill::Backend::is_running())
		quill::Backend::start();

	quill::RotatingFileSinkConfig file_config;
	file_config.set_rotation_max_file_size(kMaxFileSize);
	file_config.set_max_backup_files(kMaxBackupFiles);

	// One file sink shared by every logger: a single writer fd and one
	// rotation state, rather than several sinks racing the same path.
	auto file_sink = std::make_shared<quill::RotatingFileSink>(std::filesystem::path{settings.file}, file_config);
	std::shared_ptr<quill::Sink> console_sink;
	if (settings.console)
		console_sink = std::make_shared<quill::ConsoleSink>(quill::ConsoleSinkConfig{});

	for (auto const name : kLoggerNames) {
		// Build the sink list with reserve/push_back: a braced-init vector of
		// one element trips a GCC -Warray-bounds false positive in the
		// libstdc++ shared_ptr machinery under the sanitizer builds.
		std::vector<std::shared_ptr<quill::Sink>> sinks;
		sinks.reserve(console_sink ? 2 : 1);
		sinks.push_back(file_sink);
		if (console_sink)
			sinks.push_back(console_sink);
		quill::Frontend::create_or_get_logger(std::string{name}, std::move(sinks));
	}
}

void shutdown_logging() {
	for (auto* logger : quill::Frontend::get_all_loggers())
		logger->flush_log();
	quill::Backend::stop();
}

} // namespace araya::app
