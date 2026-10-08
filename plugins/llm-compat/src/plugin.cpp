#include "araya/llm-compat/compat.hpp"

#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace araya::llm_compat {
namespace {

// The adapter provider: apply() resolves every configured route and registers
// them on the llm service with one dispatching adapter; teardown of this fiber
// erases them.
struct compat_plugin : araya::plugin {
	explicit compat_plugin(araya::plugin_config config)
		: config(std::move(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = ctx.require<araya::llm::llm_service>(araya::llm::llm_key);
		auto routes = load_routes(config);
		if (routes.empty())
			throw std::invalid_argument("llm-compat: no provider routes configured");
		std::vector<std::string> providers;
		providers.reserve(routes.size());
		for (auto const& [id, profile] : routes)
			providers.push_back(id);
		auto adapter = std::make_shared<compat_adapter>(std::move(routes));
		registration = service->register_adapter(std::move(providers), std::move(adapter), ctx);
		co_return;
	}

	araya::plugin_config config;
	araya::registration registration;
};

std::unique_ptr<araya::plugin> make_compat(araya::plugin_config const& config) {
	return std::make_unique<compat_plugin>(config);
}

static const araya::dependency_spec g_llm_dep[]{{araya::service_id{"llm", 1}, true, {}}};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::plugin_descriptor g_descriptor{"llm-compat", g_llm_dep, g_no_provs, &make_compat};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::llm_compat
