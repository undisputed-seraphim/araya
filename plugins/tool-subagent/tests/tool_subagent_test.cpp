#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/subagents/subagents.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-subagent/tool_subagent.hpp"
#include "araya/tools/tools.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <memory>
#include <string>
#include <utility>

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
	araya::component_spec tool_spec(araya::plugin_config cfg) {
		return spec(&araya::tool_subagent::plugin_descriptor(), std::move(cfg));
	}
};

struct rig {
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<tools_service> tools;
	std::shared_ptr<araya::session::session> parent;

	araya::task<void> mount(araya::runtime& rt, harness& h, araya::plugin_config tool_cfg) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.subagents_spec());
		co_await rt.mount(h.tool_spec(std::move(tool_cfg)));
		co_await rt.wait_idle();

		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		tools = root.require<tools_service>(tools_key).shared();
		parent = store->create(root, araya::session::session_id{"parent"}, {});
	}
};

std::string text_of(tool_result const& result) {
	auto const* arr = result.content.if_array();
	if (!arr || arr->empty())
		return {};
	auto const* object = arr->front().if_object();
	if (!object)
		return {};
	auto it = object->find("text");
	return it != object->end() && it->value().is_string() ? std::string(it->value().as_string()) : std::string{};
}

} // namespace

TEST_CASE("tool-subagent registers the delegation and control tools") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, {});
		auto names = r.tools->list();
		auto has = [&](std::string_view name) {
			for (auto const& def : names)
				if (def.name == name)
					return true;
			return false;
		};
		CHECK(has("subagent"));
		CHECK(has("send_message"));
		CHECK(has("interrupt_agent"));
		CHECK(has("list_agents"));
	});
}

TEST_CASE("one-shot subagent tool returns the child output") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(
			rt, h, {{"background_mode", "one-shot"}, {"child_provider", "mock"}, {"child_model", "mock-model"}});

		auto invoked = co_await r.tools->invoke(
			"subagent",
			tool_context{
				.call_id = "c1",
				.name = "subagent",
				.session = "parent",
				.arguments = boost::json::object{{"description", "greet"}, {"prompt", "hi"}},
			});
		REQUIRE(invoked.has_value());
		CHECK_FALSE(invoked->is_error);
		CHECK(text_of(*invoked).find("echo: hi") != std::string::npos);
	});
}

TEST_CASE("continuable subagent tool starts a child and list_agents sees it") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, {{"child_provider", "mock"}, {"child_model", "mock-model"}});

		auto started = co_await r.tools->invoke(
			"subagent",
			tool_context{
				.call_id = "c1",
				.name = "subagent",
				.session = "parent",
				.arguments = boost::json::object{{"description", "greet"}, {"prompt", "hi"}},
			});
		REQUIRE(started.has_value());
		CHECK_FALSE(started->is_error);
		CHECK(text_of(*started).find("subagent started") != std::string::npos);

		// The child is created immediately; the background turn settles
		// after the harness drains the io_context.
		auto listed = co_await r.tools->invoke(
			"list_agents", tool_context{.call_id = "c2", .name = "list_agents", .session = "parent"});
		REQUIRE(listed.has_value());
		CHECK(text_of(*listed).find("running") != std::string::npos);
	});
}

TEST_CASE("subagent tool requires a calling session") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, {{"child_provider", "mock"}, {"child_model", "mock-model"}});
		auto invoked = co_await r.tools->invoke(
			"subagent",
			tool_context{
				.call_id = "c1",
				.name = "subagent",
				.arguments = boost::json::object{{"description", "greet"}, {"prompt", "hi"}},
			});
		REQUIRE(invoked.has_value());
		CHECK(invoked->is_error);
		CHECK(text_of(*invoked).find("calling session") != std::string::npos);
	});
}
