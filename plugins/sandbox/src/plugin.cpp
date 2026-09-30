#include "araya/sandbox/sandbox.hpp"

#include "araya/plugin_context.hpp"
#include "landlock.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace araya::sandbox {
namespace {

// The Linux Landlock provider. It probes the host once (cached) and builds an
// in-process confinement program per call. When Landlock is unusable it fails
// closed rather than returning the command unconfined.
//
// Escalation hook: the wider-mode retry (a model `sandbox_permissions` request
// resolved through an approval channel, then re-run under the wider policy)
// belongs here - `confine` would accept an approved mode override. It is
// deliberately absent in this trimmed port; tool-layer escalation lives at the
// same decision point in the shell handler.
struct landlock_provider final : sandbox_provider {
	landlock_provider() {
		auto const probe = detail::probe_landlock();
		available_ = probe.available;
		abi_ = probe.abi;
		reason_ = probe.reason;
	}

	bool available() const noexcept override { return available_; }
	std::string const& unavailable_reason() const noexcept override { return reason_; }

	confined confine(policy const& p) const override {
		confined out;
		out.mode = std::string(mode_name(p.mode));
		if (p.mode == sandbox_mode::danger_full_access)
			return out;
		if (!available_)
			throw sandbox_unavailable(
				"sandbox mode \"" + out.mode +
				"\" is requested but no sandbox backend is usable on this host; refusing to run the command "
				"unconfined (" +
				reason_ + ")");
		out.active = true;
		// Landlock ABI 3 added TRUNCATE; below that the mode's file-effect
		// promise is not fully enforceable, so report partial.
		out.level = abi_ >= 3 ? enforcement::full : enforcement::partial;
		out.denial_signature = "permission denied";
		out.init.mode = out.mode;
		out.init.read_only.emplace_back("/");
		out.init.read_write.emplace_back("/dev/null");
		if (p.mode == sandbox_mode::workspace_write) {
			out.init.read_write.emplace_back("/tmp");
			if (!p.workspace_root.empty() && p.workspace_root != "/" && p.workspace_root != "/tmp")
				out.init.read_write.push_back(p.workspace_root);
		}
		return out;
	}

private:
	bool available_ = false;
	std::uint32_t abi_ = 0;
	std::string reason_;
};

struct sandbox_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		std::shared_ptr<sandbox_provider> provider = std::make_shared<landlock_provider>();
		ctx.provide(sandbox_key, std::move(provider));
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_sandbox(araya::plugin_config const&) { return std::make_unique<sandbox_plugin>(); }

static constexpr std::span<araya::dependency_spec const> g_sandbox_deps{};
static const araya::provision_spec g_sandbox_provs[]{{araya::service_id{"sandbox", 1}}};
static const araya::plugin_descriptor g_descriptor{"sandbox", g_sandbox_deps, g_sandbox_provs, &make_sandbox};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::sandbox
