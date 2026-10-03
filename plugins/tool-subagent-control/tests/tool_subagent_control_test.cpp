#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/subagents/subagents.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-subagent-control/tool_subagent_control.hpp"
#include "araya/tool-subagent/tool_subagent.hpp"
#include "araya/tools/tools.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/object.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace araya::tools;
using namespace std::chrono_literals;

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
	araya::component_spec control_spec() { return spec(&araya::tool_subagent_control::plugin_descriptor()); }
	araya::component_spec delegation_spec(araya::plugin_config cfg) {
		return spec(&araya::tool_subagent::plugin_descriptor(), std::move(cfg));
	}
};

struct rig {
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<araya::subagents::subagents_service> subs;
	std::shared_ptr<tools_service> tools;
	std::shared_ptr<araya::session::session> parent;

	araya::task<void> mount_base(araya::runtime& rt, harness& h) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.subagents_spec());
		co_await rt.wait_idle();

		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		subs = root.require<araya::subagents::subagents_service>(araya::subagents::subagents_key).shared();
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

std::size_t count_name(std::vector<tool_definition> const& defs, std::string_view name) {
	std::size_t count = 0;
	for (auto const& def : defs)
		if (def.name == name)
			++count;
	return count;
}

araya::task<void> wait_ms(int ms) {
	auto ex = co_await boost::asio::this_coro::executor;
	boost::asio::steady_timer timer(ex);
	timer.expires_after(std::chrono::milliseconds(ms));
	co_await timer.async_wait(boost::asio::use_awaitable);
}

} // namespace

TEST_CASE("the control tools stay unique across delegation instances") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount_base(rt, h);
		// Two delegation tools plus one control plugin: the shared names
		// register exactly once and the per-instance names do not collide.
		co_await rt.mount(h.delegation_spec({}));
		co_await rt.mount(h.delegation_spec(
			{{"provider", "spawn"},
			 {"tool_name", "subagent_fork"},
			 {"background_mode", "one-shot"},
			 {"list_models", "false"}}));
		co_await rt.mount(h.control_spec());
		co_await rt.wait_idle();

		auto defs = r.tools->list();
		CHECK(count_name(defs, "subagent") == 1);
		CHECK(count_name(defs, "subagent_fork") == 1);
		CHECK(count_name(defs, "send_message") == 1);
		CHECK(count_name(defs, "interrupt_agent") == 1);
		CHECK(count_name(defs, "list_agents") == 1);
	});
}

TEST_CASE("send_message and interrupt_agent validate their arguments") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount_base(rt, h);
		co_await rt.mount(h.control_spec());
		co_await rt.wait_idle();

		auto send = co_await r.tools->invoke(
			"send_message", tool_context{.call_id = "c", .name = "send_message", .session = "parent"});
		REQUIRE(send.has_value());
		CHECK(send->is_error);
		CHECK(text_of(*send).find("requires 'id' and 'message'") != std::string::npos);

		auto interrupt = co_await r.tools->invoke(
			"interrupt_agent", tool_context{.call_id = "c", .name = "interrupt_agent", .session = "parent"});
		REQUIRE(interrupt.has_value());
		CHECK(interrupt->is_error);
		CHECK(text_of(*interrupt).find("requires 'id'") != std::string::npos);
	});
}

TEST_CASE("list_agents scopes and validates and reports live status") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount_base(rt, h);
		co_await rt.mount(h.control_spec());
		co_await rt.wait_idle();

		auto empty = co_await r.tools->invoke(
			"list_agents", tool_context{.call_id = "c", .name = "list_agents", .session = "parent"});
		REQUIRE(empty.has_value());
		CHECK(text_of(*empty) == "(no subagents)");

		auto bogus = co_await r.tools->invoke(
			"list_agents",
			tool_context{
				.call_id = "c",
				.name = "list_agents",
				.session = "parent",
				.arguments = boost::json::object{{"scope", "bogus"}},
			});
		REQUIRE(bogus.has_value());
		CHECK(bogus->is_error);
		CHECK(text_of(*bogus).find("children") != std::string::npos);

		// A continuable child is reported by id with a live status.
		araya::subagents::start_request req;
		req.prompt = "work";
		req.parent = "parent";
		req.label = "task";
		req.provider = "mock";
		req.model = "mock-model";
		auto started = co_await r.subs->start_continuable("spawn", std::move(req));
		REQUIRE_FALSE(started.is_error);

		auto running = co_await r.tools->invoke(
			"list_agents", tool_context{.call_id = "c", .name = "list_agents", .session = "parent"});
		REQUIRE(running.has_value());
		CHECK(text_of(*running).find(started.child) != std::string::npos);
		CHECK(text_of(*running).find("[running]") != std::string::npos);
		CHECK(text_of(*running).find("task") != std::string::npos);

		// Once the background turn settles the same child reads idle.
		co_await wait_ms(20);
		auto idle = co_await r.tools->invoke(
			"list_agents", tool_context{.call_id = "c", .name = "list_agents", .session = "parent"});
		REQUIRE(idle.has_value());
		CHECK(text_of(*idle).find("[idle]") != std::string::npos);

		auto tree = co_await r.tools->invoke(
			"list_agents",
			tool_context{
				.call_id = "c",
				.name = "list_agents",
				.session = "parent",
				.arguments = boost::json::object{{"scope", "descendants"}},
			});
		REQUIRE(tree.has_value());
		CHECK(text_of(*tree).find("parent=parent") != std::string::npos);
		CHECK(text_of(*tree).find("depth=1") != std::string::npos);

		r.subs->dispose_children();
	});
}
