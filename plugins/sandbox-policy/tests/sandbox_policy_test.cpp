#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/sandbox-policy/sandbox_policy.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>

namespace {

using namespace araya::sandbox_policy;

struct harness : araya_test::plugin_harness {
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec policy_spec(std::string mode, std::string root) {
		return spec(
			&araya::sandbox_policy::plugin_descriptor(),
			{{"mode", std::move(mode)}, {"workspace_root", std::move(root)}});
	}
};

} // namespace

TEST_CASE("the policy resolves the default mode against the calling workspace") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.policy_spec("workspace-write", "/fallback"));
		co_await rt.wait_idle();
		auto policy = rt.root_context().require<sandbox_policy_service>(sandbox_policy_key).shared();
		REQUIRE(policy != nullptr);
		CHECK(policy->default_mode() == araya::sandbox::sandbox_mode::workspace_write);

		auto const rooted = policy->resolve("/work");
		CHECK(rooted.mode == araya::sandbox::sandbox_mode::workspace_write);
		CHECK(rooted.workspace_root == "/work");

		auto const fallback = policy->resolve("");
		CHECK(fallback.workspace_root == "/fallback");
	});
}

TEST_CASE("the policy contributes the resolved policy to the runtime-context snapshot") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.policy_spec("read-only", "/fallback"));
		co_await rt.wait_idle();
		auto prompts =
			rt.root_context()
				.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key)
				.shared();

		araya::system_prompt::assemble_context context;
		context.scope = "s1";
		context.cwd = "/work";
		auto snapshot = araya::system_prompt::render_context_snapshot(prompts->assemble(context));
		CHECK(snapshot.find("read-only") != std::string::npos);
	});
}
