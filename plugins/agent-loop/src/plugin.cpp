#include "araya/agent-loop/agent.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace araya::agent {
namespace {

// The agent provider: constructs the loop over the llm, session, prompt,
// and tool services and binds it under agent_key. It also registers the
// built-in prompt variables (provider/model/cwd) with the registry.
struct agent_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto llm = ctx.require<araya::llm::llm_service>(araya::llm::llm_key).shared();
		auto store = ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		auto tools = ctx.require<araya::tools::tools_service>(araya::tools::tools_key).shared();

		prompts->variable(ctx, "provider", [](araya::system_prompt::assemble_context const& context) {
			return std::optional<std::string>(context.provider);
		});
		prompts->variable(ctx, "model", [](araya::system_prompt::assemble_context const& context) {
			return std::optional<std::string>(context.model);
		});
		prompts->variable(ctx, "cwd", [](araya::system_prompt::assemble_context const& context) {
			return std::optional<std::string>(context.cwd);
		});

		auto service =
			std::make_shared<agent_service>(std::move(llm), std::move(store), std::move(prompts), std::move(tools));
		ctx.provide(agent_key, std::move(service));
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_agent(araya::plugin_config const&) { return std::make_unique<agent_plugin>(); }

static const araya::dependency_spec g_agent_deps[]{
	{araya::service_id{"llm", 1}, true, {}},
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
};
static const araya::provision_spec g_agent_provs[]{{araya::service_id{"agent", 1}}};
static const araya::plugin_descriptor g_descriptor{"agent-loop", g_agent_deps, g_agent_provs, &make_agent};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::agent
