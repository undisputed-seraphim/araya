#include "medulla/logger/logger.hpp"
#include "medulla/plugin_context.hpp"

#include <memory>
#include <span>

namespace medulla::logger {
namespace {

log_level parse_level(std::string const& value) noexcept {
    if (value == "error")
        return log_level::error;
    if (value == "warn")
        return log_level::warn;
    if (value == "debug")
        return log_level::debug;
    return log_level::info;
}

// The logger provider: apply() constructs the service from config and
// binds it under logger_key. No dependencies.
struct logger_plugin : medulla::plugin {
    std::string name = "medulla";
    log_level level = log_level::info;

    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        auto service =
            std::make_shared<logger_service>(std::move(name), level);
        ctx.provide(logger_key, std::move(service));
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_logger(
    medulla::plugin_config const& config) {
    auto plugin = std::make_unique<logger_plugin>();
    if (auto it = config.find("name"); it != config.end() &&
                                        !it->second.empty())
        plugin->name = it->second;
    if (auto it = config.find("level"); it != config.end())
        plugin->level = parse_level(it->second);
    return plugin;
}

static constexpr std::span<medulla::dependency_spec const> g_no_deps{};
static const medulla::provision_spec g_logger_prov[]{
    {medulla::service_id{"logger", 1}}};
static const medulla::plugin_descriptor g_descriptor{
    "logger", g_no_deps, g_logger_prov, &make_logger};

}  // namespace

medulla::plugin_descriptor const& plugin_descriptor() {
    return g_descriptor;
}

}  // namespace medulla::logger
