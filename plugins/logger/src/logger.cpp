#include "araya/logger/logger.hpp"

#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <memory>
#include <utility>
#include <vector>

namespace araya::logger {

std::string render_context(log_context const& context) {
	std::string out;
	auto add = [&out](std::string_view key, std::string_view value) {
		if (value.empty())
			return;
		if (!out.empty())
			out += ' ';
		out += key;
		out += '=';
		out += value;
	};
	add("ses", context.session);
	if (context.turn)
		add("t", std::to_string(context.turn));
	if (context.step)
		add("s", std::to_string(context.step));
	add("call", context.call);
	if (out.empty())
		return {};
	return "[" + out + "]";
}

namespace {

std::vector<std::shared_ptr<quill::Sink>>& default_sinks() {
	static std::vector<std::shared_ptr<quill::Sink>> sinks;
	return sinks;
}

} // namespace

void set_default_sinks(std::vector<std::shared_ptr<quill::Sink>> sinks) { default_sinks() = std::move(sinks); }

std::string safe_url(std::string_view url) {
	auto const scheme_end = url.find("://");
	if (scheme_end == std::string_view::npos) {
		auto const cut = url.find_first_of("?#");
		return std::string(url.substr(0, cut));
	}
	std::string out(url.substr(0, scheme_end + 3));
	auto rest = url.substr(scheme_end + 3);
	auto const authority_end = rest.find_first_of("/?#");
	auto authority = rest.substr(0, authority_end);
	if (auto const at = authority.rfind('@'); at != std::string_view::npos)
		authority.remove_prefix(at + 1);
	out += authority;
	if (authority_end != std::string_view::npos) {
		auto const path = rest.substr(authority_end);
		auto const cut = path.find_first_of("?#");
		out += path.substr(0, cut);
	}
	return out;
}

std::optional<log_level> parse_level(std::string_view word) {
	if (word == "error")
		return log_level::error;
	if (word == "warn")
		return log_level::warn;
	if (word == "info")
		return log_level::info;
	if (word == "debug")
		return log_level::debug;
	return std::nullopt;
}

namespace {

quill::LogLevel to_quill(log_level level) noexcept {
	switch (level) {
	case log_level::error:
		return quill::LogLevel::Error;
	case log_level::warn:
		return quill::LogLevel::Warning;
	case log_level::info:
		return quill::LogLevel::Info;
	case log_level::debug:
		return quill::LogLevel::Debug;
	}
	return quill::LogLevel::Info;
}

} // namespace

logger_service::logger_service(std::string default_name, log_level level)
	: default_name_(std::move(default_name))
	, level_(level) {
	// Quill does not auto-start its backend on first logger creation; the
	// first logger service in the process starts it (idempotent).
	if (!quill::Backend::is_running())
		quill::Backend::start(quill::BackendOptions{});
	get_or_create(default_name_);
}

named_logger logger_service::root() const {
	return named_logger(const_cast<logger_service&>(*this), *backend(default_name_), default_name_);
}

named_logger logger_service::named(std::string name) {
	auto* backend = get_or_create(name);
	return named_logger(*this, *backend, std::move(name));
}

quill::Logger* logger_service::get_or_create(std::string const& name) {
	auto found = loggers_.find(name);
	if (found != loggers_.end())
		return found->second;

	// Adopt an existing quill logger (the app pre-creates the root logger
	// with its shared file/console sinks), else create one with the
	// registered default sinks so plugin loggers land in the same file.
	// With no defaults (tests/demos) keep the console fallback.
	quill::Logger* backend = quill::Frontend::get_logger(name);
	if (!backend) {
		auto const& sinks = default_sinks();
		if (!sinks.empty()) {
			backend = quill::Frontend::create_or_get_logger(name, sinks);
		} else {
			backend = quill::Frontend::create_or_get_logger(
				name, std::make_shared<quill::ConsoleSink>(quill::ConsoleSinkConfig{}));
		}
	}
	backend->set_log_level(to_quill(level_));
	loggers_.emplace(name, backend);
	return backend;
}

quill::Logger* logger_service::backend(std::string const& name) const {
	auto found = loggers_.find(name);
	return found == loggers_.end() ? nullptr : found->second;
}

void logger_service::set_level(log_level level) noexcept {
	level_ = level;
	for (auto const& [name, backend] : loggers_)
		backend->set_log_level(to_quill(level));
}

void logger_service::submit(quill::Logger* backend, log_level level, std::string text) const {
	if (!backend)
		return;
	switch (level) {
	case log_level::error:
		QUILL_LOG_ERROR(backend, "{}", text);
		break;
	case log_level::warn:
		QUILL_LOG_WARNING(backend, "{}", text);
		break;
	case log_level::info:
		QUILL_LOG_INFO(backend, "{}", text);
		break;
	case log_level::debug:
		QUILL_LOG_DEBUG(backend, "{}", text);
		break;
	}
}

} // namespace araya::logger
