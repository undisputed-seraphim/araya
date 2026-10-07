#include "araya/lsp-stdio/lsp_stdio.hpp"

#include "araya/config.hpp"
#include "araya/logger/logger.hpp"
#include "araya/lsp/lsp.hpp"
#include "araya/plugin_context.hpp"

#include "config.hpp"
#include "host.hpp"
#include "provider.hpp"

#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace araya::lsp_stdio {
namespace {

constexpr araya::config_key<std::string> config_file_key{"config_file"};
constexpr araya::config_key<std::string> config_inline_key{"config"};

constexpr araya::config_field g_config[] = {
	field(config_file_key, "Path to the LSP servers JSON config file."),
	field(config_inline_key, "Inline LSP servers JSON config."),
};

std::unique_ptr<araya::plugin> make_lsp_stdio(araya::plugin_config const& config) {
	struct lsp_stdio_plugin : araya::plugin {
		explicit lsp_stdio_plugin(araya::plugin_config const& cfg)
			: config(cfg) {}

		araya::task<void> apply(araya::plugin_context& ctx) override {
			auto lsp = ctx.require<araya::lsp::lsp_service>(araya::lsp::lsp_key).shared();
			auto parsed = parse_config(config);

			// An optional logger for graceful-degradation warnings.
			std::shared_ptr<araya::logger::logger_service> logger;
			if (auto lease = ctx.find<araya::logger::logger_service>(araya::logger::logger_key))
				logger = lease->shared();
			auto warn = [logger](std::string const& message) {
				if (logger)
					logger->named("lsp-stdio").warn("{}", message);
			};

			// Resolve each command now and skip one that is not installed, so a
			// missing toolchain degrades gracefully instead of failing the whole
			// stack (the default servers are commonly absent).
			std::vector<std::shared_ptr<stdio_lsp_provider>> providers;
			providers.reserve(parsed.servers.size());
			for (auto& server : parsed.servers) {
				if (!find_executable(server.command)) {
					warn("lsp-stdio: skipping '" + server.id + "': '" + server.command + "' is not installed");
					continue;
				}
				providers.push_back(std::make_shared<stdio_lsp_provider>(ctx.executor(), std::move(server)));
			}

			// The disposal effect is registered before the provider routes so,
			// at teardown (LIFO), routes are removed first and no new query can
			// enter a draining provider.
			ctx.effect([providers]() -> araya::cleanup_action {
				return [providers] {
					for (auto const& provider : providers)
						provider->dispose();
				};
			});

			std::vector<araya::registration> registrations;
			try {
				for (auto const& provider : providers)
					registrations.push_back(lsp->register_provider(ctx, provider));
			} catch (...) {
				for (auto& registration : registrations)
					registration.release();
				throw;
			}
			co_return;
		}

		araya::plugin_config config;
	};
	return std::make_unique<lsp_stdio_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"lsp", 1}, true, {}},
	// Optional: used only to warn when a configured server is not installed.
	{araya::service_id{"logger", 1}, false, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"lsp-stdio", g_deps, g_provs, &make_lsp_stdio, g_config};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::lsp_stdio
