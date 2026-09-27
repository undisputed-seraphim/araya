#include "araya/goal/goal.hpp"

#include "araya/config.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace araya::goal {
namespace {

constexpr araya::config_key<std::uint64_t> default_max_goal_rounds_key{"defaultMaxGoalRounds"};

// The goal provider: constructs the service over the session store, registers
// the `goal` projection, and binds the service under goals_key.
struct goal_plugin : araya::plugin {
	explicit goal_plugin(araya::plugin_config config)
		: config_(std::move(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto store = ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		std::uint64_t default_rounds = 256;
		if (auto configured = araya::plugin_config_view(config_).try_get(default_max_goal_rounds_key))
			default_rounds = *configured;

		araya::session::projection_state<goal_projection_state> projection;
		(void)store->register_projection(ctx, goal_projection(), projection);

		auto service = std::make_shared<goal_service>(
			ctx.activation_ptr() ? ctx.activation_ptr()->bus : nullptr,
			std::move(store),
			std::move(projection),
			default_rounds);
		ctx.provide(goals_key, std::move(service));
		co_return;
	}

private:
	araya::plugin_config config_;
};

std::unique_ptr<araya::plugin> make_goal(araya::plugin_config const& config) {
	return std::make_unique<goal_plugin>(config);
}

static const araya::dependency_spec g_goal_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
};
static const araya::provision_spec g_goal_provs[]{{araya::service_id{"goals", 1}}};
static const araya::plugin_descriptor g_descriptor{"goal", g_goal_deps, g_goal_provs, &make_goal};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::goal
