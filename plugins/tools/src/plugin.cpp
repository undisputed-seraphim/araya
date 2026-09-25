#include "araya/tools/tools.hpp"

#include <memory>
#include <span>
#include <utility>

namespace araya::tools {
namespace {

// The tool provider: apply() constructs the registry and registers its
// schema provider with the system-prompt service, so tool schemas feed
// every assembly. Requires system-prompt.
struct tools_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = std::make_shared<tools_service>();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		prompts->tools(ctx, [service](araya::system_prompt::assemble_context const& context) {
			return service->schemas(context.scope);
		});
		ctx.provide(tools_key, std::move(service));
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_tools(araya::plugin_config const&) { return std::make_unique<tools_plugin>(); }

static const araya::dependency_spec g_tools_deps[]{{araya::service_id{"system-prompt", 1}, true, {}}};
static const araya::provision_spec g_tools_provs[]{{araya::service_id{"tools", 1}}};
static const araya::plugin_descriptor g_descriptor{"tools", g_tools_deps, g_tools_provs, &make_tools};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tools
