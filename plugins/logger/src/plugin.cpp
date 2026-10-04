#include "araya/config.hpp"
#include "araya/logger/logger.hpp"
#include "araya/plugin_context.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace araya {

// The word-valued log level, parsed through the config customization
// point; unknown spellings fall back to info (the plugin default).
template <>
struct config_parser<logger::log_level> {
	static std::optional<logger::log_level> parse(std::string_view text) { return logger::parse_level(text); }
};

} // namespace araya

namespace araya::logger {
namespace {

inline constexpr araya::config_key<log_level> level_key{"level"};

constexpr araya::config_field g_config[] = {
	{"name", "Default logger name.", "araya", false, nullptr},
	field(level_key, "Log threshold (error|warn|info|debug).", "info"),
};

// The logger provider: apply() constructs the service from config and
// binds it under logger_key. No dependencies.
struct logger_plugin : araya::plugin {
	std::string name = "araya";
	log_level level = log_level::info;

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = std::make_shared<logger_service>(std::move(name), level);
		ctx.provide(logger_key, std::move(service));
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_logger(araya::plugin_config const& config) {
	auto plugin = std::make_unique<logger_plugin>();
	if (auto it = config.find("name"); it != config.end() && !it->second.empty())
		plugin->name = it->second;
	if (auto level = araya::plugin_config_view(config).try_get(level_key))
		plugin->level = *level;
	return plugin;
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static const araya::provision_spec g_logger_prov[]{{araya::service_id{"logger", 1}}};
static const araya::plugin_descriptor g_descriptor{"logger", g_no_deps, g_logger_prov, &make_logger, g_config};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::logger
