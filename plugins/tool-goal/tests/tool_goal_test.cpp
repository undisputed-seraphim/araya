#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/goal/goal.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tool-goal/tool_goal.hpp"
#include "araya/tools/tools.hpp"
#include "support/plugin_harness.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace araya::goal;

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
	araya::component_spec goal_spec() { return spec(&araya::goal::plugin_descriptor()); }
	araya::component_spec tool_goal_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::tool_goal::plugin_descriptor(), std::move(cfg));
	}
};

struct rig {
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<araya::tools::tools_service> tools;
	std::shared_ptr<goal_service> goals;
	std::shared_ptr<araya::agent::agent_service> agent;
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts;
	std::shared_ptr<araya::session::session> session;

	araya::task<void> mount(araya::runtime& rt, harness& h, araya::plugin_config tool_cfg = {}) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.goal_spec());
		co_await rt.mount(h.tool_goal_spec(std::move(tool_cfg)));
		co_await rt.wait_idle();
		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		tools = root.require<araya::tools::tools_service>(araya::tools::tools_key).shared();
		goals = root.require<goal_service>(goals_key).shared();
		agent = root.require<araya::agent::agent_service>(araya::agent::agent_key).shared();
		prompts =
			root.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		session = store->create(root, araya::session::session_id{"s1"}, {});
	}

	araya::task<std::optional<araya::tools::tool_result>> call(std::string name, boost::json::object arguments = {}) {
		co_return co_await tools->invoke(
			name,
			araya::tools::tool_context{
				.call_id = "c",
				.name = name,
				.session = "s1",
				.arguments = std::move(arguments),
			});
	}
};

std::string text_of(araya::tools::tool_result const& result) {
	auto const* array = result.content.if_array();
	if (!array || array->empty())
		return {};
	auto const* object = array->front().if_object();
	if (!object)
		return {};
	auto it = object->find("text");
	return it != object->end() && it->value().is_string() ? std::string(it->value().as_string()) : std::string{};
}

// Open a synthetic model turn for s1 and admit one human message.
void open_human_turn(rig& r) {
	r.session->append("turn/start", boost::json::value{{"turn", 1}});
	r.session->append(
		"user/message",
		araya::llm_bridge::user_message_data("u1", "please create a goal", araya::llm_bridge::message_source("user")));
}

// Admit one goal-round message for the current goal.
void admit_goal_round(rig& r, std::uint64_t round) {
	auto goal = r.goals->get(araya::session::session_id{"s1"});
	if (!goal)
		return;
	r.session->append(
		"user/message",
		araya::llm_bridge::user_message_data(
			"g" + std::to_string(round),
			"round " + std::to_string(round),
			araya::llm_bridge::message_source(
				"goal", {{"goalId", goal->id}, {"revision", goal->revision}, {"round", round}})));
}

} // namespace

TEST_CASE("tool-goal registers its tools and the tool:goal section") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		auto listed = r.tools->list();
		auto has = [&](std::string_view name) {
			return std::any_of(listed.begin(), listed.end(), [&](auto const& t) { return t.name == name; });
		};
		CHECK(has("get_goal"));
		CHECK(has("create_goal"));
		CHECK(has("update_goal"));

		araya::system_prompt::assemble_context context;
		context.scope = "s1";
		context.provider = "mock";
		context.model = "m";
		auto rendered = araya::system_prompt::render_prompt(r.prompts->assemble(context));
		CHECK(rendered.find("consecutive goal rounds") != std::string::npos);
	});
}

TEST_CASE("goal tools require an open model turn") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		bool threw = false;
		try {
			(void)co_await r.call("get_goal");
		} catch (std::exception const& e) {
			threw = true;
			CHECK(std::string(e.what()).find("open model turn") != std::string::npos);
		}
		CHECK(threw);
	});
}

TEST_CASE("a direct human turn can create, read, and complete a goal") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		open_human_turn(r);

		auto created = co_await r.call("create_goal", {{"objective", "ship the release"}});
		REQUIRE(created.has_value());
		CHECK_FALSE(created->is_error);
		auto created_text = text_of(*created);
		CHECK(created_text.find("\"phase\":\"active\"") != std::string::npos);
		CHECK(created_text.find("\"activation\":\"armed\"") != std::string::npos);

		auto goal = r.goals->get(araya::session::session_id{"s1"});
		REQUIRE(goal.has_value());

		auto read = co_await r.call("get_goal");
		REQUIRE(read.has_value());
		CHECK(text_of(*read).find(goal->id) != std::string::npos);

		auto completed = co_await r.call(
			"update_goal", {{"goal_id", goal->id}, {"revision", goal->revision}, {"action", "complete"}});
		REQUIRE(completed.has_value());
		CHECK(text_of(*completed).find("\"phase\":\"complete\"") != std::string::npos);
	});
}

TEST_CASE("create_goal rejects a turn with no direct human input") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		// An open turn whose only message is a non-human plugin notice.
		r.session->append("turn/start", boost::json::value{{"turn", 1}});
		r.session->append(
			"user/message",
			araya::llm_bridge::user_message_data(
				"p1", "plugin notice", araya::llm_bridge::message_source("plugin", {{"plugin", "x"}})));

		bool threw = false;
		try {
			(void)co_await r.call("create_goal", {{"objective", "nope"}});
		} catch (std::exception const& e) {
			threw = true;
			CHECK(std::string(e.what()).find("direct human") != std::string::npos);
		}
		CHECK(threw);
	});
}

TEST_CASE("blocked under goal-round authority needs the minimum rounds") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		open_human_turn(r);
		auto created = co_await r.call("create_goal", {{"objective", "long task"}});
		REQUIRE(created.has_value());
		auto goal = r.goals->get(araya::session::session_id{"s1"});
		REQUIRE(goal.has_value());

		// Start a fresh turn carrying exactly the first goal round.
		r.session->append("turn/end", boost::json::value{{"turn", 1}, {"reason", "completed"}});
		r.session->append("turn/start", boost::json::value{{"turn", 2}});
		admit_goal_round(r, 1);

		bool threw = false;
		try {
			(void)co_await r.call(
				"update_goal",
				{{"goal_id", goal->id},
				 {"revision", goal->revision},
				 {"action", "blocked"},
				 {"blocked_reason", "waiting"}});
		} catch (std::exception const& e) {
			threw = true;
			CHECK(std::string(e.what()).find("consecutive goal rounds") != std::string::npos);
		}
		CHECK(threw);

		// complete is allowed under the same goal-round authority (no threshold).
		auto completed = co_await r.call(
			"update_goal", {{"goal_id", goal->id}, {"revision", goal->revision}, {"action", "complete"}});
		REQUIRE(completed.has_value());
		CHECK(text_of(*completed).find("\"phase\":\"complete\"") != std::string::npos);
	});
}
