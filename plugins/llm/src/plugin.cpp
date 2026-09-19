#include "araya/llm/llm.hpp"

#include <memory>
#include <span>
#include <stdexcept>

namespace araya::llm {
namespace {

// The llm provider: apply() constructs the service on the owning
// activation's bus executor and binds it under llm_key. Adapters
// register routes through the service from their own fibers.
struct llm_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		if (!ctx.activation_ptr() || !ctx.activation_ptr()->bus)
			throw std::logic_error("llm requires an activation with an event bus");
		auto service = std::make_shared<llm_service>(ctx.activation_ptr()->bus->executor());
		ctx.provide(llm_key, std::move(service));
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_llm(araya::plugin_config const&) { return std::make_unique<llm_plugin>(); }

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static const araya::provision_spec g_llm_prov[]{{araya::service_id{"llm", 1}}};
static const araya::plugin_descriptor g_descriptor{"llm", g_no_deps, g_llm_prov, &make_llm};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::llm
