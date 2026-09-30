#pragma once

#include "araya/plugin.hpp"
#include "araya/sandbox/sandbox.hpp"
#include "araya/service.hpp"

#include <string>
#include <string_view>

// The sandbox policy home: the deployment's default file-effect mode and the
// workspace root resolution. The one shared policy every enforcing consumer
// reads, so bash (and, eventually, the filesystem tools) resolve the same
// mode and root. A trimmed port of the harness's
// `@deepseek-ai/dsh-sandbox-policy`: the deployment default plus the calling
// session's workspace. Per-session runtime overrides (a `sandbox/mode` event
// and projection) are out of scope here.
namespace araya::sandbox_policy {

class sandbox_policy_service {
public:
	sandbox_policy_service(araya::sandbox::sandbox_mode default_mode, std::string fallback_root)
		: default_mode_(default_mode)
		, fallback_root_(std::move(fallback_root)) {}

	// The deployment default mode a session starts from.
	araya::sandbox::sandbox_mode default_mode() const noexcept { return default_mode_; }

	// The root used when the calling session carries no cwd.
	std::string const& fallback_root() const noexcept { return fallback_root_; }

	// Resolve the policy for one call: the deployment default mode, rooted at
	// the calling session's workspace (`workspace_root`, empty to use the
	// configured fallback).
	araya::sandbox::policy resolve(std::string_view workspace_root) const {
		return araya::sandbox::policy{
			.mode = default_mode_,
			.workspace_root = workspace_root.empty() ? fallback_root_ : std::string(workspace_root)};
	}

private:
	araya::sandbox::sandbox_mode default_mode_;
	std::string fallback_root_;
};

inline constexpr araya::service_key<sandbox_policy_service> sandbox_policy_key{"sandbox-policy", 1};

// The plugin descriptor: requires `system-prompt` (the runtime-context
// snapshot), provides {"sandbox-policy",1}.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::sandbox_policy
