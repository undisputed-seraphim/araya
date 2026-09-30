#include "araya/sandbox-policy/sandbox_policy.hpp"

#include "araya/config.hpp"
#include "araya/plugin_context.hpp"
#include "araya/system-prompt/system_prompt.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace araya::sandbox_policy {
namespace {

constexpr araya::config_key<std::string> mode_key{"mode"};
constexpr araya::config_key<std::string> root_key{"workspace_root"};

// Render the resolved policy without claiming which capabilities are mounted
// (the tool layer owns the operation-specific denial guidance).
std::string render_policy(araya::sandbox::sandbox_mode mode, std::string const& root) {
	switch (mode) {
	case araya::sandbox::sandbox_mode::read_only:
		return "Current file sandbox policy: read-only. The bash tool cannot modify files in this standing mode. "
			   "Do not refuse a required modification from this policy alone: try the operation normally and follow "
			   "any denial guidance its result returns.";
	case araya::sandbox::sandbox_mode::workspace_write:
		return "Current file sandbox policy: workspace-write. The bash tool may modify files under the session "
			   "workspace: " +
			   (root.empty() ? std::string("(the session workspace)") : root) +
			   ". Some platform temporary areas may also be writable.";
	case araya::sandbox::sandbox_mode::danger_full_access:
		return "Current file sandbox policy: danger-full-access. The bash tool is not restricted by the file "
			   "sandbox.";
	}
	return {};
}

struct sandbox_policy_plugin : araya::plugin {
	explicit sandbox_policy_plugin(araya::plugin_config const& config) { parse(config); }

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();

		std::shared_ptr<sandbox_policy_service> service = std::make_shared<sandbox_policy_service>(mode_, root_);
		ctx.provide(sandbox_policy_key, std::move(service));

		// The resolved policy rides the cache-safe runtime-context snapshot;
		// the agent loop logs it as model history, so it lands without
		// rewriting the stable system prompt.
		araya::system_prompt::prompt_context context;
		context.name = "sandbox";
		context.order = araya::system_prompt::context_order("SANDBOX_POLICY");
		araya::sandbox::sandbox_mode const mode = mode_;
		std::string const fallback = root_;
		context.render = [mode, fallback](araya::system_prompt::assemble_context const& a) {
			return render_policy(mode, a.cwd.empty() ? fallback : a.cwd);
		};
		prompts->context(ctx, std::move(context));
		co_return;
	}

private:
	void parse(araya::plugin_config const& config) {
		araya::plugin_config_view view(config);
		if (auto value = view.try_get(mode_key)) {
			if (auto parsed = araya::sandbox::parse_mode(*value))
				mode_ = *parsed;
		}
		if (auto value = view.try_get(root_key))
			root_ = *value;
		if (root_.empty())
			root_ = std::filesystem::current_path().string();
	}

	araya::sandbox::sandbox_mode mode_ = araya::sandbox::sandbox_mode::workspace_write;
	std::string root_;
};

std::unique_ptr<araya::plugin> make_sandbox_policy(araya::plugin_config const& config) {
	return std::make_unique<sandbox_policy_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"system-prompt", 1}, true, {}},
};
static const araya::provision_spec g_provs[]{{araya::service_id{"sandbox-policy", 1}}};
static const araya::plugin_descriptor g_descriptor{"sandbox-policy", g_deps, g_provs, &make_sandbox_policy};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::sandbox_policy
