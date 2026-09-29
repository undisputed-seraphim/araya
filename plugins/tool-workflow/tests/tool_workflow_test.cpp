#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/subagents/subagents.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-workflow/tool_workflow.hpp"
#include "araya/tools/tools.hpp"
#include "araya/workflow/workflow.hpp"

#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>

namespace {
using namespace araya::tools;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec llm_spec() { return spec(&araya::llm::plugin_descriptor()); }
	araya::component_spec mock_spec() {
		return spec(&araya::llm_mock::plugin_descriptor(), {{"provider", "mock"}, {"response", "echo: {user}"}});
	}
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec agent_spec() { return spec(&araya::agent::plugin_descriptor()); }
	araya::component_spec subagents_spec() { return spec(&araya::subagents::plugin_descriptor()); }
	araya::component_spec workflow_spec() { return spec(&araya::workflow::plugin_descriptor()); }
	araya::component_spec tool_spec(araya::plugin_config cfg) {
		return spec(&araya::tool_workflow::plugin_descriptor(), std::move(cfg));
	}

	// Mount everything the workflow engine needs, then the model-facing tool.
	araya::task<void> mount_stack(araya::runtime& rt, araya::plugin_config tool_cfg) {
		co_await rt.mount(session_spec());
		co_await rt.mount(llm_spec());
		co_await rt.mount(mock_spec());
		co_await rt.mount(prompt_spec());
		co_await rt.mount(tools_spec());
		co_await rt.mount(agent_spec());
		co_await rt.mount(subagents_spec());
		co_await rt.mount(workflow_spec());
		co_await rt.mount(tool_spec(std::move(tool_cfg)));
		co_await rt.wait_idle();
	}
};

bool has_tool(araya::runtime& rt, std::string_view name) {
	auto tools = rt.root_context().require<tools_service>(tools_key).shared();
	for (auto const& definition : tools->list())
		if (definition.name == name)
			return true;
	return false;
}

} // namespace

TEST_CASE("the workflow tool is absent unless enabled") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await h.mount_stack(rt, {});
		CHECK_FALSE(has_tool(rt, "workflow"));
	});
}

TEST_CASE("enabling the workflow tool registers it with its guidance section") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await h.mount_stack(rt, {{"enabled", "true"}});
		CHECK(has_tool(rt, "workflow"));

		auto prompts =
			rt.root_context()
				.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key)
				.shared();
		araya::system_prompt::assemble_context context;
		bool found = false;
		for (auto const& section : prompts->assemble(context).sections) {
			if (section.name == "tool:workflow") {
				found = true;
				CHECK(section.text.find("workflow") != std::string::npos);
			}
		}
		CHECK(found);
	});
}
