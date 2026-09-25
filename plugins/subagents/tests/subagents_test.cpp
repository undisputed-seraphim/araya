#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/subagents/subagents.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/serialize.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace araya::subagents;
using namespace std::chrono_literals;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec llm_spec() { return spec(&araya::llm::plugin_descriptor()); }
	araya::component_spec mock_spec(araya::plugin_config cfg = {{"provider", "mock"}, {"response", "echo: {user}"}}) {
		return spec(&araya::llm_mock::plugin_descriptor(), std::move(cfg));
	}
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec agent_spec() { return spec(&araya::agent::plugin_descriptor()); }
	araya::component_spec subagents_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::subagents::plugin_descriptor(), std::move(cfg));
	}
};

struct rig {
	harness& h;
	std::shared_ptr<araya::session::session_store> store;
	std::shared_ptr<subagents_service> subs;
	std::shared_ptr<araya::session::session> parent;

	araya::task<void> mount(araya::runtime& rt, araya::plugin_config sub_cfg = {}) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.subagents_spec(std::move(sub_cfg)));
		co_await rt.wait_idle();

		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		subs = root.require<subagents_service>(subagents_key).shared();
		parent = store->create(root, araya::session::session_id{"parent"}, {});
	}
};

std::size_t count_type(araya::session::session const& s, std::string_view type) {
	std::size_t count = 0;
	for (auto const& event : s.log()) {
		if (event.type == type)
			++count;
	}
	return count;
}

// Yields to the control strand until `predicate` holds or the budget runs
// out; a background child turn needs the strand to make progress.
araya::task<void> wait_until(std::function<bool()> predicate, int max_ms = 1000) {
	auto ex = co_await boost::asio::this_coro::executor;
	boost::asio::steady_timer timer(ex);
	for (int i = 0; i < max_ms && !predicate(); ++i) {
		timer.expires_after(1ms);
		co_await timer.async_wait(boost::asio::use_awaitable);
	}
}

} // namespace

TEST_CASE("spawn one-shot runs a child and returns its output") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(rt);

		start_request req;
		req.prompt = "hello";
		req.parent = "parent";
		req.provider = "mock";
		req.model = "mock-model";
		auto result = co_await r.subs->run("spawn", std::move(req));

		CHECK_FALSE(result.is_error);
		CHECK(result.stop_reason == "completed");
		CHECK(result.output == "echo: hello");
		REQUIRE_FALSE(result.child.empty());

		auto child = r.store->get(araya::session::session_id{result.child});
		REQUIRE(child);
		CHECK(child->header().origin == araya::session::session_origin::subagent);
		CHECK(child->header().delegation_depth == 1);
		REQUIRE(child->header().parent_session.has_value());
		CHECK(child->header().parent_session->value == "parent");
	});
}

TEST_CASE("unknown provider and missing route fail loud") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(rt);

		start_request req;
		req.prompt = "x";
		req.parent = "parent";
		auto unknown = co_await r.subs->run("nope", req);
		CHECK(unknown.is_error);
		CHECK(unknown.error.find("unknown provider") != std::string::npos);

		// No route override and no inherited request header in the parent.
		auto no_route = co_await r.subs->run("spawn", req);
		CHECK(no_route.is_error);
		CHECK(no_route.error.find("requires a provider and model") != std::string::npos);
	});
}

TEST_CASE("continuable start settles into the parent session") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(rt);

		start_request req;
		req.prompt = "work";
		req.parent = "parent";
		req.label = "task";
		req.provider = "mock";
		req.model = "mock-model";
		auto result = co_await r.subs->start_continuable("spawn", std::move(req));

		CHECK_FALSE(result.is_error);
		CHECK(result.stop_reason == "running");
		REQUIRE_FALSE(result.child.empty());

		co_await wait_until([&] { return count_type(*r.parent, "user/message") >= 1; });
		CHECK(count_type(*r.parent, "user/message") == 1);

		auto children = r.subs->list_children("parent");
		REQUIRE(children.size() == 1);
		CHECK(children[0].id == result.child);
		CHECK(children[0].label == "task");
		CHECK(children[0].running == false);
		CHECK(children[0].stop_reason == "completed");

		// The settlement carries the child's output.
		auto const* settlement = [&]() -> boost::json::value const* {
			for (auto it = r.parent->log().rbegin(); it != r.parent->log().rend(); ++it)
				if (it->type == "user/message")
					return &it->data;
			return nullptr;
		}();
		REQUIRE(settlement != nullptr);
		CHECK(boost::json::serialize(*settlement).find("echo: work") != std::string::npos);
	});
}

TEST_CASE("send_message continues an idle child") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(rt);

		start_request req;
		req.prompt = "first";
		req.parent = "parent";
		req.provider = "mock";
		req.model = "mock-model";
		auto started = co_await r.subs->start_continuable("spawn", std::move(req));
		REQUIRE_FALSE(started.is_error);
		co_await wait_until([&] { return count_type(*r.parent, "user/message") >= 1; });

		auto sent = co_await r.subs->send_message(started.child, "second");
		CHECK_FALSE(sent.is_error);
		co_await wait_until([&] { return count_type(*r.parent, "user/message") >= 2; });
		CHECK(count_type(*r.parent, "user/message") == 2);

		auto unknown = co_await r.subs->send_message("ghost", "x");
		CHECK(unknown.is_error);
		CHECK_FALSE(r.subs->interrupt("ghost"));
	});
}

TEST_CASE("depth limit refuses a too-deep child") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(rt);

		auto root = rt.root_context();
		araya::session::create_session_options deep;
		deep.delegation_depth = 1;
		r.store->create(root, araya::session::session_id{"deep"}, deep);

		start_request req;
		req.prompt = "too deep";
		req.parent = "deep";
		auto result = co_await r.subs->run("spawn", std::move(req));
		CHECK(result.is_error);
		CHECK(result.error.find("depth limit") != std::string::npos);
	});
}
