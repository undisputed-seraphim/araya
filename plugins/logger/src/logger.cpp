#include "araya/logger/logger.hpp"

#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/ConsoleSink.h>

#include <memory>
#include <utility>

namespace araya::logger {

namespace {

quill::LogLevel to_quill(log_level level) noexcept {
    switch (level) {
        case log_level::error: return quill::LogLevel::Error;
        case log_level::warn: return quill::LogLevel::Warning;
        case log_level::info: return quill::LogLevel::Info;
        case log_level::debug: return quill::LogLevel::Debug;
    }
    return quill::LogLevel::Info;
}

}  // namespace

logger_service::logger_service(std::string default_name, log_level level)
    : default_name_(std::move(default_name)), level_(level) {
    // Quill does not auto-start its backend on first logger creation; the
    // first logger service in the process starts it (idempotent).
    if (!quill::Backend::is_running())
        quill::Backend::start(quill::BackendOptions{});
    get_or_create(default_name_);
}

named_logger logger_service::root() const {
    return named_logger(const_cast<logger_service*>(this),
                        backend(default_name_), default_name_);
}

named_logger logger_service::named(std::string name) {
    auto* backend = get_or_create(name);
    return named_logger(this, backend, std::move(name));
}

quill::Logger* logger_service::get_or_create(std::string const& name) {
    auto found = loggers_.find(name);
    if (found != loggers_.end())
        return found->second;

    // Adopt an existing quill logger (power users pre-create one with
    // custom sinks) or create one with the console sink.
    quill::Logger* backend = quill::Frontend::get_logger(name);
    if (!backend) {
        backend = quill::Frontend::create_or_get_logger(
            name, std::make_shared<quill::ConsoleSink>(
                      quill::ConsoleSinkConfig{}));
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

void logger_service::submit(quill::Logger* backend, log_level level,
                            std::string text) const {
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

}  // namespace araya::logger
