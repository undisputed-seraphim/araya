#include "araya/logger/logger.hpp"
#include "araya/plugin_context.hpp"

#include <memory>
#include <span>

namespace araya::logger {
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
struct logger_plugin : araya::plugin {
    std::string name = "araya";
    log_level level = log_level::info;

    araya::task<void> apply(araya::plugin_context& ctx) override {
        auto service =
            std::make_shared<logger_service>(std::move(name), level);
        ctx.provide(logger_key, std::move(service));
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_logger(
    araya::plugin_config const& config) {
    auto plugin = std::make_unique<logger_plugin>();
    if (auto it = config.find("name"); it != config.end() &&
                                        !it->second.empty())
        plugin->name = it->second;
    if (auto it = config.find("level"); it != config.end())
        plugin->level = parse_level(it->second);
    return plugin;
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static const araya::provision_spec g_logger_prov[]{
    {araya::service_id{"logger", 1}}};
static const araya::plugin_descriptor g_descriptor{
    "logger", g_no_deps, g_logger_prov, &make_logger};

}  // namespace

araya::plugin_descriptor const& plugin_descriptor() {
    return g_descriptor;
}

}  // namespace araya::logger
