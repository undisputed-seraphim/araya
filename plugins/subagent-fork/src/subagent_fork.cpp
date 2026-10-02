#include "araya/subagent-fork/subagent_fork.hpp"

#include "araya/config.hpp"
#include "araya/plugin_context.hpp"
#include "araya/session/store.hpp"
#include "araya/subagents/detail/child.hpp"
#include "araya/subagents/subagents.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace araya::subagent_fork {
namespace {

constexpr araya::config_key<std::string> provider_name_key{"provider_name"};

// The balanced completed-turn prefix of the parent's log: every event up to
// and including the last `turn/end`. The in-flight turn is excluded because it
// is unbalanced and cannot be replayed as a valid child session. Before any
// completed turn the prefix is empty and the child starts fresh.
std::vector<araya::session::session_event> completed_turn_prefix(araya::session::session const& parent) {
	auto const& log = parent.log();
	std::size_t cut = 0;
	for (std::size_t index = 0; index < log.size(); ++index) {
		if (log[index].type == "turn/end")
			cut = index + 1;
	}
	return std::vector<araya::session::session_event>(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(cut));
}

// The fork backend: a child seeded with the parent's completed-turn prefix, at
// parent depth + 1, inheriting the parent's cwd and preset.
class fork_provider final : public araya::subagents::subagent_provider {
public:
	explicit fork_provider(std::shared_ptr<araya::session::session_store> store)
		: store_(std::move(store)) {}

	araya::subagents::capabilities caps() const override {
		araya::subagents::capabilities caps;
		caps.inherits_parent_context = true;
		return caps;
	}

	std::shared_ptr<araya::session::session>
	create_child(araya::subagents::start_request const& req, std::uint32_t child_depth) override {
		using namespace araya::session;
		auto parent = store_->get(session_id{req.parent});
		if (!parent)
			throw std::invalid_argument("subagent-fork: unknown parent session '" + req.parent + "'");

		auto options = araya::subagents::detail::child_session_options(req.parent, child_depth, parent.get());
		auto seed = completed_turn_prefix(*parent);
		if (!seed.empty()) {
			options.inherited_event_count = static_cast<session_log_offset>(seed.size());
			options.is_seeded = true;
			options.seed = std::move(seed);
		}

		return araya::subagents::detail::enter_child(*store_, std::move(options));
	}

private:
	std::shared_ptr<araya::session::session_store> store_;
};

struct subagent_fork_plugin : araya::plugin {
	explicit subagent_fork_plugin(araya::plugin_config const& config) { parse(config); }

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto store = ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto subs = ctx.require<araya::subagents::subagents_service>(araya::subagents::subagents_key).shared();
		subs->register_provider(ctx, provider_name_, std::make_shared<fork_provider>(std::move(store)));
		co_return;
	}

private:
	void parse(araya::plugin_config const& config) {
		araya::plugin_config_view view(config);
		if (auto value = view.try_get(provider_name_key))
			provider_name_ = *value;
	}

	std::string provider_name_ = "fork";
};

std::unique_ptr<araya::plugin> make_subagent_fork(araya::plugin_config const& config) {
	return std::make_unique<subagent_fork_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"subagents", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"subagent-fork", g_deps, g_provs, &make_subagent_fork};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::subagent_fork
