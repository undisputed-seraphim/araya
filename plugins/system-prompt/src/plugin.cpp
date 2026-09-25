#include "araya/system-prompt/system_prompt.hpp"

#include "araya/config.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace araya::system_prompt {
namespace {

inline constexpr araya::config_key<std::string> persona_prefix_key{"persona_prefix"};
inline constexpr araya::config_key<std::string> persona_suffix_key{"persona_suffix"};
inline constexpr araya::config_key<bool> include_harness_identity_key{"include_harness_identity"};
inline constexpr araya::config_key<bool> include_runtime_context_key{"include_runtime_context"};

// The prompt registry: apply() constructs the service, seeds it from the
// plugin config (persona, identity/runtime-context switches, tool order),
// and binds it under system_prompt_key. No dependencies - feature plugins
// contribute through the registry themselves.
struct system_prompt_plugin : araya::plugin {
	explicit system_prompt_plugin(araya::plugin_config config)
		: config(std::move(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = std::make_shared<system_prompt_service>();
		araya::plugin_config_view const view(config);
		if (auto value = view.try_get(persona_prefix_key))
			service->set_persona_prefix(*value);
		if (auto value = view.try_get(persona_suffix_key))
			service->set_persona_suffix(*value);
		if (auto value = view.try_get(include_harness_identity_key))
			service->set_include_harness_identity(*value);
		if (auto value = view.try_get(include_runtime_context_key))
			service->set_include_runtime_context(*value);
		if (auto const found = config.find("tool_order"); found != config.end() && !found->second.empty()) {
			auto const parsed = boost::json::parse(found->second);
			if (!parsed.is_array())
				throw std::invalid_argument("system-prompt: tool_order must be a JSON array");
			std::vector<std::string> order;
			for (auto const& entry : parsed.as_array()) {
				if (!entry.is_string())
					throw std::invalid_argument("system-prompt: tool_order entries must be strings");
				order.emplace_back(entry.as_string());
			}
			service->set_tool_order(std::move(order));
		}
		ctx.provide(system_prompt_key, std::move(service));
		co_return;
	}

	araya::plugin_config config;
};

std::unique_ptr<araya::plugin> make_system_prompt(araya::plugin_config const& config) {
	return std::make_unique<system_prompt_plugin>(config);
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static const araya::provision_spec g_system_prompt_provs[]{{araya::service_id{"system-prompt", 1}}};
static const araya::plugin_descriptor g_descriptor{
	"system-prompt",
	g_no_deps,
	g_system_prompt_provs,
	&make_system_prompt};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::system_prompt
