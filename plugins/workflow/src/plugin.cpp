#include "araya/workflow/workflow.hpp"

#include "araya/config.hpp"
#include "process_worker.hpp"
#include "worker_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace araya::workflow {
namespace {

constexpr araya::config_key<std::string> node_key{"node_executable"};
constexpr araya::config_key<std::uint64_t> max_message_key{"max_message_bytes"};
constexpr araya::config_key<std::uint64_t> max_concurrency_key{"max_concurrency"};
constexpr araya::config_key<std::uint64_t> max_total_agents_key{"max_total_agents"};

constexpr araya::config_field g_config[] = {
	field(node_key, "Node executable for the workflow worker.", "node"),
	field(max_message_key, "Maximum control-frame/queued-write size in bytes.", "4194304"),
	field(max_concurrency_key, "Agents run at once; further calls queue.", "4"),
	field(max_total_agents_key, "Ceiling on accepted agent() calls per run.", "64"),
};

config parse_config(araya::plugin_config const& raw) {
	araya::plugin_config_view const view(raw);
	config out;
	if (auto value = view.try_get(node_key))
		out.node_executable = *value;
	if (auto value = view.try_get(max_message_key))
		out.max_message_bytes = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(max_concurrency_key))
		out.max_concurrency = static_cast<std::uint32_t>(*value);
	if (auto value = view.try_get(max_total_agents_key))
		out.max_total_agents = static_cast<std::uint32_t>(*value);
	return out;
}

struct workflow_plugin : araya::plugin {
	explicit workflow_plugin(araya::plugin_config config)
		: config_(parse_config(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto store = ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto subagents = ctx.require<araya::subagents::subagents_service>(araya::subagents::subagents_key).shared();

		auto service = std::make_shared<workflow_service>(ctx.executor(), store, subagents, config_);
		auto const executor = ctx.executor();
		auto const cfg = config_;
		service->set_worker_factory([executor, cfg]() -> std::shared_ptr<worker> {
			return std::make_shared<process_worker>(
				executor, cfg.node_executable, std::string(workflow_worker_source()), cfg.max_message_bytes);
		});
		ctx.effect([service]() -> araya::cleanup_action { return [service] { service->shutdown(); }; });
		ctx.provide(workflow_key, std::move(service));
		co_return;
	}

private:
	config config_;
};

std::unique_ptr<araya::plugin> make_workflow(araya::plugin_config const& config) {
	return std::make_unique<workflow_plugin>(config);
}

static const araya::dependency_spec g_workflow_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"subagents", 1}, true, {}},
};
static const araya::provision_spec g_workflow_provs[]{{araya::service_id{"workflow", 1}}};
static const araya::plugin_descriptor
	g_descriptor{"workflow", g_workflow_deps, g_workflow_provs, &make_workflow, g_config};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::workflow
