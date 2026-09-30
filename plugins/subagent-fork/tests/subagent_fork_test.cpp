#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/subagent-fork/subagent_fork.hpp"
#include "araya/subagents/subagents.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace araya::subagents;

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
	araya::component_spec fork_spec() { return spec(&araya::subagent_fork::plugin_descriptor()); }
};

// Append one balanced completed turn (user + assistant + turn/end) to a session.
void append_completed_turn(araya::session::session& s) {
	using boost::json::object;
	using boost::json::value;
	s.append(
		"user/message",
		value{object{
			{"id", "u1"},
			{"role", "user"},
			{"content", boost::json::array{{object{{"type", "text"}, {"text", "hi"}}}}}}});
	s.append(
		"assistant/message",
		value{object{
			{"message",
			 object{
				 {"id", "a1"},
				 {"role", "assistant"},
				 {"content", boost::json::array{{object{{"type", "text"}, {"text", "hello"}}}}}}}}});
	s.append("turn/end", value{object{{"turn", 1}, {"reason", "completed"}}});
}

bool has_type(araya::session::session const& s, std::string_view type) {
	return std::any_of(s.log().begin(), s.log().end(), [&](auto const& e) { return e.type == type; });
}

} // namespace

TEST_CASE("the fork provider is registered by name") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.subagents_spec());
		co_await rt.mount(h.fork_spec());
		co_await rt.wait_idle();
		auto subs = rt.root_context().require<subagents_service>(subagents_key).shared();
		auto names = subs->provider_names();
		CHECK(std::find(names.begin(), names.end(), "fork") != names.end());
		CHECK(subs->find_provider("fork") != nullptr);
	});
}

TEST_CASE("a forked child is seeded with the parent's completed-turn prefix") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.subagents_spec());
		co_await rt.mount(h.fork_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		auto store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto subs = root.require<subagents_service>(subagents_key).shared();

		auto parent = store->create(root, araya::session::session_id{"parent"}, {});
		append_completed_turn(*parent);
		std::vector<std::string> const prefix_types = {"user/message", "assistant/message", "turn/end"};

		start_request req;
		req.prompt = "continue";
		req.parent = "parent";
		req.provider = "mock";
		req.model = "mock-model";
		auto result = co_await subs->run("fork", std::move(req));
		REQUIRE_FALSE(result.is_error);

		auto child = store->get(araya::session::session_id{result.child});
		REQUIRE(child != nullptr);
		// The child opens with exactly the parent's completed-turn prefix...
		REQUIRE(child->log().size() > prefix_types.size());
		for (std::size_t i = 0; i < prefix_types.size(); ++i)
			CHECK(child->log()[i].type == prefix_types[i]);
		// ...closed by the inherited-seed marker.
		CHECK(child->log()[prefix_types.size()].type == "session/end-seed");
		// The child is one delegation level below the parent.
		CHECK(child->header().delegation_depth == parent->header().delegation_depth + 1);
	});
}

TEST_CASE("a fork with no completed turn behaves like a fresh spawn") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.subagents_spec());
		co_await rt.mount(h.fork_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		auto store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto subs = root.require<subagents_service>(subagents_key).shared();
		store->create(root, araya::session::session_id{"parent"}, {});

		start_request req;
		req.prompt = "start";
		req.parent = "parent";
		req.provider = "mock";
		req.model = "mock-model";
		auto result = co_await subs->run("fork", std::move(req));
		REQUIRE_FALSE(result.is_error);

		auto child = store->get(araya::session::session_id{result.child});
		REQUIRE(child != nullptr);
		CHECK_FALSE(has_type(*child, "session/end-seed"));
	});
}
