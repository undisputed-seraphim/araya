#include "araya/llm-openai/openai.hpp"

#include <memory>
#include <span>

namespace araya::llm_openai {
namespace {

// The adapter provider: apply() registers the configured route with the
// llm service; teardown of this fiber erases it.
struct openai_plugin : araya::plugin {
	explicit openai_plugin(openai_config config)
		: config(std::move(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = ctx.require<araya::llm::llm_service>(araya::llm::llm_key);
		auto adapter = std::make_shared<openai_adapter>(config);
		registration = service->register_adapter({config.provider}, std::move(adapter), ctx);
		co_return;
	}

	openai_config config;
	araya::registration registration;
};

std::unique_ptr<araya::plugin> make_openai(araya::plugin_config const& config) {
	// load_config throws std::invalid_argument on bad config: the fiber
	// fails to load and the host reports the error.
	return std::make_unique<openai_plugin>(load_config(config));
}

static const araya::dependency_spec g_llm_dep[]{{araya::service_id{"llm", 1}, true, {}}};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::plugin_descriptor g_descriptor{"llm-openai", g_llm_dep, g_no_provs, &make_openai};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::llm_openai
