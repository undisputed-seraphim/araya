#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/goal-round-driver/goal_round_driver.hpp"
#include "araya/goal/goal.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace araya::goal;
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
	araya::component_spec goal_spec() { return spec(&araya::goal::plugin_descriptor()); }
	araya::component_spec driver_spec() { return spec(&araya::goal_round_driver::plugin_descriptor()); }
};

struct rig {
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<goal_service> goals;
	std::shared_ptr<araya::session::session> session;

	araya::task<void> mount(araya::runtime& rt, harness& h) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.goal_spec());
		co_await rt.mount(h.driver_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		goals = root.require<goal_service>(goals_key).shared();
		session = store->create(root, araya::session::session_id{"s1"}, {});
	}
};

araya::task<void> wait_until(std::function<bool()> predicate, int max_ms = 1000) {
	auto ex = co_await boost::asio::this_coro::executor;
	boost::asio::steady_timer timer(ex);
	for (int i = 0; i < max_ms && !predicate(); ++i) {
		timer.expires_after(1ms);
		co_await timer.async_wait(boost::asio::use_awaitable);
	}
}

bool saw_goal_round(araya::session::session const& session) {
	for (auto const& message : session.surface().messages()) {
		if (araya::llm_bridge::message_text(message).find("<goal_round>") != std::string::npos)
			return true;
	}
	return false;
}

} // namespace

TEST_CASE("an armed goal drives a round and blocks at its round limit") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		auto goal = r.goals->create(araya::session::session_id{"s1"}, "finish the task", 1);
		CHECK(goal.activation == goal_activation::armed);

		co_await wait_until([&] {
			auto current = r.goals->get(araya::session::session_id{"s1"});
			return current && current->phase == goal_phase::blocked;
		});

		auto current = r.goals->get(araya::session::session_id{"s1"});
		REQUIRE(current.has_value());
		CHECK(current->phase == goal_phase::blocked);
		REQUIRE(current->blocked_reason.has_value());
		CHECK(current->blocked_reason->code == "round-limit");
		CHECK(current->rounds_started == 1);
		CHECK(saw_goal_round(*r.session));
	});
}
