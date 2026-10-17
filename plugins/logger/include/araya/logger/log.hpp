#pragma once

#include "araya/logger/logger.hpp"
#include "araya/plugin_context.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

// The optional-plugin logging helper: a handle a plugin resolves once in
// apply() and then uses from listeners and handlers. An absent logger
// (tests, or a host that did not mount it) makes every call a no-op, and a
// filtered level skips formatting. Logger names are component ids; session
// and call context ride in log_context instead of creating per-session
// loggers.
namespace araya::logger {

class log_handle {
public:
	log_handle() = default;

	log_handle(std::shared_ptr<logger_service> service, std::string name)
		: service_(std::move(service))
		, name_(std::move(name)) {}

	explicit operator bool() const noexcept { return static_cast<bool>(service_); }

	bool enabled(log_level level) const noexcept { return service_ && service_->enabled(level); }

	template <class... Args>
	void debug(std::string_view fmt, Args&&... args) const {
		emit(log_level::debug, nullptr, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void debug(log_context const& context, std::string_view fmt, Args&&... args) const {
		emit(log_level::debug, &context, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void info(std::string_view fmt, Args&&... args) const {
		emit(log_level::info, nullptr, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void info(log_context const& context, std::string_view fmt, Args&&... args) const {
		emit(log_level::info, &context, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void warn(std::string_view fmt, Args&&... args) const {
		emit(log_level::warn, nullptr, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void warn(log_context const& context, std::string_view fmt, Args&&... args) const {
		emit(log_level::warn, &context, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void error(std::string_view fmt, Args&&... args) const {
		emit(log_level::error, nullptr, fmt, std::forward<Args>(args)...);
	}

	template <class... Args>
	void error(log_context const& context, std::string_view fmt, Args&&... args) const {
		emit(log_level::error, &context, fmt, std::forward<Args>(args)...);
	}

private:
	template <class... Args>
	void emit(log_level level, log_context const* context, std::string_view fmt, Args&&... args) const {
		if (!enabled(level))
			return;
		auto logger = service_->named(name_);
		if (context)
			logger.log(level, *context, fmt, std::forward<Args>(args)...);
		else
			logger.log(level, fmt, std::forward<Args>(args)...);
	}

	std::shared_ptr<logger_service> service_;
	std::string name_;
};

// Resolves the optional service once; empty when the logger is not mounted.
inline log_handle from_context(araya::plugin_context& ctx, std::string name) {
	if (auto lease = ctx.find<logger_service>(logger_key))
		return log_handle(lease->shared(), std::move(name));
	return {};
}

} // namespace araya::logger
