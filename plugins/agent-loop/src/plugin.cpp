#include "araya/agent-loop/agent.hpp"
#include "araya/agent-loop/events.hpp"
#include "araya/agent-loop/inbox.hpp"
#include "araya/session/events.hpp"

#include <boost/json/object.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace araya::agent {
namespace {

boost::json::value lifecycle_source(araya::session::session const& session) {
	return boost::json::object{{"kind", session.header().is_seeded ? "resume" : "startup"}};
}

// The agent provider: constructs the loop over the llm, session, prompt,
// and tool services and binds it under agent_key. It registers the built-in
// prompt variables (provider/model/cwd), the durable inbox and turnBoundary
// projections, and the session lifecycle listeners that create and dispose
// drivers.
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

		// The durable inbox and the turn/wait-step boundary fold are
		// store-driven projections owned by this fiber; the trackers are the
		// service's read handles.
		araya::session::projection_state<inbox_state> inbox;
		(void)store->register_projection(ctx, inbox_projection(), inbox);
		araya::session::projection_state<turn_state> turn_boundary;
		(void)store->register_projection(ctx, turn_boundary_projection(), turn_boundary);

		auto service = std::make_shared<agent_service>(
			ctx.activation_ptr() ? ctx.activation_ptr()->bus : nullptr,
			ctx.scope(),
			ctx.executor(),
			std::move(llm),
			std::move(store),
			std::move(prompts),
			std::move(tools),
			std::move(inbox),
			std::move(turn_boundary));

		// Lifecycle: a session acquires a driver when it is announced and
		// loses it when it is disposed. Both listeners are owned by this
		// fiber and removed at teardown.
		auto handle = service;
		ctx.on(
			araya::session::created_key,
			[handle](araya::session::session_created_msg const& message) -> araya::task<void> {
				co_await handle->ensure(*message.s, lifecycle_source(*message.s));
			});
		ctx.on(
			araya::session::disposed_key,
			[handle](araya::session::session_disposed_msg const& message) -> araya::task<void> {
				co_await handle->dispose(message.id);
			});
		ctx.effect([handle]() -> araya::cleanup_action { return [handle] { handle->shutdown(); }; });

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
