#include "araya/agent-loop/agent.hpp"

#include "araya/config.hpp"

#include <memory>
#include <span>
#include <utility>

namespace araya::agent {
namespace {

inline constexpr araya::config_key<std::string> system_prompt_key{"system_prompt"};

// The agent provider: apply() constructs the loop over the llm and
// session services and binds it under agent_key. The configured system
// prompt (config key "system_prompt", empty by default) rides along -
// no prompt text is hardcoded anywhere.
struct agent_plugin : araya::plugin {
	explicit agent_plugin(araya::plugin_config config)
		: config(std::move(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto llm = ctx.require<araya::llm::llm_service>(araya::llm::llm_key).shared();
		auto store = ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto service = std::make_shared<agent_service>(std::move(llm), std::move(store));
		if (auto value = araya::plugin_config_view(config).try_get(system_prompt_key))
			service->set_system_prompt(*value);
		ctx.provide(agent_key, std::move(service));
		co_return;
	}

	araya::plugin_config config;
};

std::unique_ptr<araya::plugin> make_agent(araya::plugin_config const& config) {
	return std::make_unique<agent_plugin>(config);
}

static const araya::dependency_spec g_agent_deps[]{
	{araya::service_id{"llm", 1}, true, {}},
	{araya::service_id{"sessions", 1}, true, {}},
};
static const araya::provision_spec g_agent_provs[]{{araya::service_id{"agent", 1}}};
static const araya::plugin_descriptor g_descriptor{"agent-loop", g_agent_deps, g_agent_provs, &make_agent};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::agent
