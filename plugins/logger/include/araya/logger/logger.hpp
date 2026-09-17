#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"

#include <quill/Backend.h>
#include <quill/Logger.h>

#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace araya::logger {

// The four severity levels the service exposes. They map onto quill's
// Debug/Info/Warning/Error; quill's frontend performs the gating, so a
// filtered call costs one branch before the queue push.
enum class log_level : std::uint8_t { error, warn, info, debug };

class logger_service;

// A named logger: the handle a plugin obtains from logger_service::named()
// or root(). Log calls are synchronous: the record is formatted on the
// calling thread (std::vformat) and submitted to quill's frontend, which
// is the only asynchronous step in the chain — quill's own queue and
// backend thread.
class named_logger {
public:
    template <class... Args>
    void error(std::string_view fmt, Args&&... args) const {
        log(log_level::error, fmt, std::forward<Args>(args)...);
    }

    template <class... Args>
    void warn(std::string_view fmt, Args&&... args) const {
        log(log_level::warn, fmt, std::forward<Args>(args)...);
    }

    template <class... Args>
    void info(std::string_view fmt, Args&&... args) const {
        log(log_level::info, fmt, std::forward<Args>(args)...);
    }

    template <class... Args>
    void debug(std::string_view fmt, Args&&... args) const {
        log(log_level::debug, fmt, std::forward<Args>(args)...);
    }

private:
    friend class logger_service;

    named_logger(logger_service* service, quill::Logger* backend,
                 std::string name)
        : service_(service), backend_(backend), name_(std::move(name)) {}

    template <class... Args>
    void log(log_level level, std::string_view fmt, Args&&... args) const;

    logger_service* service_;
    quill::Logger* backend_;
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
    explicit logger_service(std::string default_name,
                            log_level level = log_level::info);

    // The default logger (the plugin config's name).
    named_logger root() const;

    // A logger for the given name, created on first use.
    named_logger named(std::string name);

    void set_level(log_level level) noexcept;

    log_level level() const noexcept { return level_; }

    // The underlying quill logger, for attaching custom sinks or quill
    // features directly. Returns null when the name is unknown.
    quill::Logger* backend(std::string const& name) const;

private:
    friend class named_logger;

    quill::Logger* get_or_create(std::string const& name);

    // Formats nothing: the named_logger templates format with std::vformat
    // and hand over the rendered text. Never throws.
    void submit(quill::Logger* backend, log_level level,
                std::string text) const;

    std::string default_name_;
    std::map<std::string, quill::Logger*> loggers_;
    log_level level_;
};

inline constexpr araya::service_key<logger_service> logger_key{"logger",
                                                                 1};

// The plugin descriptor: apply() constructs the service (default logger
// name and level from config) and provides it under logger_key.
araya::plugin_descriptor const& plugin_descriptor();

template <class... Args>
void named_logger::log(log_level level, std::string_view fmt,
                       Args&&... args) const {
    std::string text;
    try {
        text = std::vformat(fmt, std::make_format_args(args...));
    } catch (std::format_error const&) {
        // A bad format string or an unformattable argument never throws
        // into the caller's log statement.
        text = std::string(fmt) + " <format error>";
    }
    service_->submit(backend_, level, std::move(text));
}

}  // namespace araya::logger
