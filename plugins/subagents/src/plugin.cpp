#include "araya/subagents/subagents.hpp"

#include "araya/agent-loop/events.hpp"
#include "araya/config.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace araya::subagents {
namespace {

// The deployment ceiling on a call's `max_depth` override. The built-in
// spawn provider is registered here; future providers register on the
// service through the seam.
constexpr araya::config_key<std::uint32_t> max_depth_key{"max_depth"};

struct subagents_plugin : araya::plugin {
	explicit subagents_plugin(araya::plugin_config config)
		: config_(std::move(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto store = ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto agent = ctx.require<araya::agent::agent_service>(araya::agent::agent_key).shared();
		auto tools = ctx.require<araya::tools::tools_service>(araya::tools::tools_key).shared();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();

		std::uint32_t max_depth = 1;
		if (auto configured = araya::plugin_config_view(config_).try_get(max_depth_key))
			max_depth = *configured;

		auto service = std::make_shared<subagents_service>(ctx.executor(), store, agent, tools, prompts, max_depth);
		auto activation = ctx.activation_ptr();
		service->set_context_factory([activation] { return araya::plugin_context{activation}; });
		service->register_provider(ctx, "spawn", make_spawn_provider(store));
		// A child is an ordinary driver session; its settlement is delivered
		// when that driver goes idle. Observing the transition (rather than
		// running a private turn loop) is what keeps this seam thin.
		ctx.on(araya::agent::agent_status_key, [service](araya::agent::agent_status_msg const& message) {
			if (message.status == araya::agent::agent_status::idle)
				service->on_agent_idle(message.session);
		});
		ctx.effect([service]() -> araya::cleanup_action { return [service] { service->dispose_children(); }; });
		ctx.provide(subagents_key, std::move(service));
		co_return;
	}

private:
	araya::plugin_config config_;
};

std::unique_ptr<araya::plugin> make_subagents(araya::plugin_config const& config) {
	return std::make_unique<subagents_plugin>(config);
}

static const araya::dependency_spec g_subagents_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"agent", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
};
static const araya::provision_spec g_subagents_provs[]{{araya::service_id{"subagents", 1}}};
static const araya::plugin_descriptor g_descriptor{"subagents", g_subagents_deps, g_subagents_provs, &make_subagents};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::subagents
