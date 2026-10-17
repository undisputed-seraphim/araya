#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"

#include <quill/Backend.h>
#include <quill/Logger.h>
#include <quill/sinks/Sink.h>

#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::logger {

// The four severity levels the service exposes. They map onto quill's
// Debug/Info/Warning/Error; quill's frontend performs the gating, so a
// filtered call costs one branch before the queue push.
enum class log_level : std::uint8_t { error, warn, info, debug };

// Correlation fields attached to one record. Views must remain valid for the
// duration of the call (construct one inline from live ids). `turn`/`step`
// are omitted when zero.
struct log_context {
	std::string_view session;
	std::string_view call;
	std::uint64_t turn = 0;
	std::uint64_t step = 0;
};

// Renders a context as `[ses=… t=… s=… call=…]`, or an empty string.
std::string render_context(log_context const& context);

// The process-wide sinks used when a logger name is created on demand. The
// app registers its shared file/console sinks before boot so plugin loggers
// land in the same file; when empty, the service keeps the console fallback
// (tests and demos).
void set_default_sinks(std::vector<std::shared_ptr<quill::Sink>> sinks);

// Strips userinfo, query, and fragment from a URL so an endpoint can be
// logged without leaking credentials or tokens.
std::string safe_url(std::string_view url);

// Parses a level word ("error"|"warn"|"info"|"debug"); nullopt on an
// unknown spelling. Shared by the config parser and flag/env handling.
std::optional<log_level> parse_level(std::string_view word);

class logger_service;

// A named logger: the handle a plugin obtains from logger_service::named()
// or root(). Log calls are synchronous: the record is formatted on the
// calling thread (std::vformat) and submitted to quill's frontend, which
// is the only asynchronous step in the chain — quill's own queue and
// backend thread.
class named_logger {
public:
	// Whether a record at this level would be emitted (the cheap gate that
	// lets callers skip formatting work entirely).
	bool enabled(log_level level) const noexcept;

	// The generic form the small `log_handle` helper (araya/logger/log.hpp)
	// forwards to; the named methods below are the usual call sites.
	template <class... Args>
	void log(log_level level, std::string_view fmt, Args&&... args) const;

	template <class... Args>
	void log(log_level level, log_context const& context, std::string_view fmt, Args&&... args) const;

	template <class... Args>
	void debug(std::string_view fmt, Args&&... args) const {
		log(log_level::debug, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void debug(log_context const& context, std::string_view fmt, Args&&... args) const {
		log(log_level::debug, context, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void info(std::string_view fmt, Args&&... args) const {
		log(log_level::info, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void info(log_context const& context, std::string_view fmt, Args&&... args) const {
		log(log_level::info, context, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void warn(std::string_view fmt, Args&&... args) const {
		log(log_level::warn, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void warn(log_context const& context, std::string_view fmt, Args&&... args) const {
		log(log_level::warn, context, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void error(std::string_view fmt, Args&&... args) const {
		log(log_level::error, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void error(log_context const& context, std::string_view fmt, Args&&... args) const {
		log(log_level::error, context, fmt, std::forward<Args>(args)...);
	}

private:
	friend class logger_service;

	named_logger(logger_service& service, quill::Logger& backend, std::string name)
		: service_(service)
		, backend_(backend)
		, name_(std::move(name)) {}

	// Both are borrowed, never null: the service owns the handle, quill
	// owns the backend.
	logger_service& service_;
	quill::Logger& backend_;
	std::string name_;
};

// The logger service: quill built in, no backend abstraction. It owns one
// quill::Logger per named handle (created on first use with a console
// sink, or reused when the name already exists — power users can
// pre-create a quill logger with custom sinks and the service adopts it).
// The level threshold is applied to every known logger through quill's own
// per-logger gating.
class logger_service {
public:
	explicit logger_service(std::string default_name, log_level level = log_level::info);

	// The default logger (the plugin config's name).
	named_logger root() const;

	// A logger for the given name, created on first use.
	named_logger named(std::string name);

	void set_level(log_level level) noexcept;

	log_level level() const noexcept { return level_; }

	// Whether a record at this level would pass the threshold. Callers use
	// this to skip formatting work for filtered statements.
	bool enabled(log_level level) const noexcept {
		return static_cast<unsigned>(level) <= static_cast<unsigned>(level_);
	}

	// The underlying quill logger, for attaching custom sinks or quill
	// features directly. Returns null when the name is unknown.
	quill::Logger* backend(std::string const& name) const;

private:
	friend class named_logger;

	quill::Logger* get_or_create(std::string const& name);

	// Formats nothing: the named_logger templates format with std::vformat
	// and hand over the rendered text. Never throws.
	void submit(quill::Logger* backend, log_level level, std::string text) const;

	std::string default_name_;
	std::map<std::string, quill::Logger*> loggers_;
	log_level level_;
};

inline constexpr araya::service_key<logger_service> logger_key{"logger", 1};

inline bool named_logger::enabled(log_level level) const noexcept { return service_.enabled(level); }

namespace detail {

// Formats one record; a bad format string or an unformattable argument never
// throws into the caller's log statement.
template <class... Args>
std::string format_message(std::string_view fmt, Args&&... args) {
	try {
		return std::vformat(fmt, std::make_format_args(args...));
	} catch (std::format_error const&) {
		return std::string(fmt) + " <format error>";
	}
}

} // namespace detail

// The plugin descriptor: apply() constructs the service (default logger
// name and level from config) and provides it under logger_key.
araya::plugin_descriptor const& plugin_descriptor();

template <class... Args>
void named_logger::log(log_level level, std::string_view fmt, Args&&... args) const {
	if (!service_.enabled(level))
		return;
	service_.submit(&backend_, level, detail::format_message(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void named_logger::log(log_level level, log_context const& context, std::string_view fmt, Args&&... args) const {
	if (!service_.enabled(level))
		return;
	std::string text = render_context(context);
	std::string message = detail::format_message(fmt, std::forward<Args>(args)...);
	if (text.empty()) {
		text = std::move(message);
	} else {
		text += ' ';
		text += message;
	}
	service_.submit(&backend_, level, std::move(text));
}

} // namespace araya::logger
