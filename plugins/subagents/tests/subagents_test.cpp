#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/bridge.hpp"
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
#include <boost/json/parse.hpp>
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

	araya::task<void> mount(
		araya::runtime& rt,
		araya::plugin_config sub_cfg = {},
		araya::plugin_config mock_cfg = {{"provider", "mock"}, {"response", "echo: {user}"}}) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec(std::move(mock_cfg)));
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

std::vector<std::string> user_texts(araya::session::session const& s) {
	std::vector<std::string> out;
	for (auto const& message : s.surface().messages()) {
		if (message.role == araya::session::message_role::user)
			out.push_back(araya::llm_bridge::message_text(message));
	}
	return out;
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

TEST_CASE("send_message steers a running child inside its current turn") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		// A paced reply keeps the child's first turn in flight long enough
		// for the send to take the running branch.
		co_await r.mount(rt, {}, {{"provider", "mock"}, {"response", "echo: {user}"}, {"delay_ms", "50"}});

		start_request req;
		req.prompt = "first";
		req.parent = "parent";
		req.provider = "mock";
		req.model = "mock-model";
		auto started = co_await r.subs->start_continuable("spawn", std::move(req));
		REQUIRE_FALSE(started.is_error);

		auto sent = co_await r.subs->send_message(started.child, "steer");
		REQUIRE_FALSE(sent.is_error);
		CHECK(sent.stop_reason == "queued");

		co_await wait_until([&] { return count_type(*r.parent, "user/message") >= 1; });

		// The steer joined the running turn: one turn, both inputs.
		auto child = r.store->get(araya::session::session_id{started.child});
		REQUIRE(child);
		CHECK(count_type(*child, "turn/start") == 1);
		// The first prompt carries the child's delegation-context snapshot, so
		// the task inputs are the last two user messages.
		auto texts = user_texts(*child);
		REQUIRE(texts.size() >= 2);
		CHECK(texts[texts.size() - 2] == "first");
		CHECK(texts[texts.size() - 1] == "steer");
	});
}

namespace {

struct no_schema_provider : subagent_provider {
	capabilities caps() const override {
		capabilities c;
		c.output_schema = false;
		return c;
	}
	std::shared_ptr<araya::session::session> create_child(start_request const&, std::uint32_t) override {
		return nullptr;
	}
};

boost::json::value object_schema() {
	return boost::json::parse(
		R"({"type":"object","properties":{"answer":{"type":"number"}},"required":["answer"],"additionalProperties":false})");
}

start_request structured_request(std::string prompt) {
	start_request req;
	req.prompt = std::move(prompt);
	req.parent = "parent";
	req.provider = "mock";
	req.model = "mock-model";
	req.output_schema = object_schema();
	return req;
}

} // namespace

TEST_CASE("a structured one-shot captures and validates the child result") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(
			rt,
			{},
			{{"provider", "mock"},
			 {"script",
			  R"([{"tool_call":{"name":"structured_output","arguments":"{\"answer\":42}"}},{"text":"done"}])"}});

		auto result = co_await r.subs->run("spawn", structured_request("answer the question"));

		CHECK_FALSE(result.is_error);
		CHECK(result.stop_reason == "completed");
		REQUIRE(result.structure.has_value());
		CHECK(result.structure->at("answer").as_int64() == 42);
	});
}

TEST_CASE("a completed child that never records is an error") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(rt); // default mock replies with plain text

		auto result = co_await r.subs->run("spawn", structured_request("just talk"));

		CHECK(result.is_error);
		CHECK(result.stop_reason == "error");
		CHECK(result.error.find("not recorded") != std::string::npos);
	});
}

TEST_CASE("invalid structured arguments are rejected so the child can retry") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(
			rt,
			{},
			{{"provider", "mock"},
			 {"script",
			  R"([{"tool_call":{"name":"structured_output","arguments":"{\"wrong\":1}"}},{"tool_call":{"name":"structured_output","arguments":"{\"answer\":7}"}},{"text":"done"}])"}});

		auto result = co_await r.subs->run("spawn", structured_request("answer"));

		CHECK_FALSE(result.is_error);
		REQUIRE(result.structure.has_value());
		CHECK(result.structure->at("answer").as_int64() == 7);
	});
}

TEST_CASE("a provider without structured-output support is rejected") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(rt);
		auto root = rt.root_context();
		r.subs->register_provider(root, "no-schema", std::make_shared<no_schema_provider>());

		auto result = co_await r.subs->run("no-schema", structured_request("answer"));

		CHECK(result.is_error);
		CHECK(result.error.find("does not support structured output") != std::string::npos);
	});
}

TEST_CASE("consecutive structured children keep their own captures") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(
			rt,
			{},
			{{"provider", "mock"},
			 {"script",
			  R"([{"tool_call":{"name":"structured_output","arguments":"{\"answer\":1}"}},{"text":"one"},{"tool_call":{"name":"structured_output","arguments":"{\"answer\":2}"}},{"text":"two"}])"}});

		auto first = co_await r.subs->run("spawn", structured_request("first"));
		auto second = co_await r.subs->run("spawn", structured_request("second"));

		REQUIRE(first.structure.has_value());
		REQUIRE(second.structure.has_value());
		CHECK(first.structure->at("answer").as_int64() == 1);
		CHECK(second.structure->at("answer").as_int64() == 2);
	});
}

TEST_CASE("a child carries the delegation scope note and an optional persona") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(rt);
		auto root = rt.root_context();
		auto prompts =
			root.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();

		start_request req;
		req.prompt = "hello";
		req.parent = "parent";
		req.provider = "mock";
		req.model = "mock-model";
		req.persona = "You are a narrow auditor.";
		auto result = co_await r.subs->start_continuable("spawn", std::move(req));
		REQUIRE_FALSE(result.is_error);
		REQUIRE_FALSE(result.child.empty());

		araya::system_prompt::assemble_context child_ctx;
		child_ctx.scope = result.child;
		auto assembly = prompts->assemble(child_ctx);
		bool delegation = false;
		bool persona = false;
		for (auto const& context : assembly.contexts)
			if (context.name == "subagent:delegation")
				delegation = true;
		for (auto const& section : assembly.sections)
			if (section.name == "deployment:persona-prefix" && section.text == "You are a narrow auditor.")
				persona = true;
		CHECK(delegation);
		CHECK(persona);

		// The parent scope keeps the deployment persona, not the child's shadow.
		araya::system_prompt::assemble_context parent_ctx;
		parent_ctx.scope = std::string("parent");
		bool parent_shadowed = false;
		for (auto const& section : prompts->assemble(parent_ctx).sections)
			if (section.name == "deployment:persona-prefix" && section.text == "You are a narrow auditor.")
				parent_shadowed = true;
		CHECK_FALSE(parent_shadowed);

		r.subs->dispose_children();
	});
}

TEST_CASE("a child tool filter naming an unknown global fails the run") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r{h};
		co_await r.mount(rt);

		start_request req;
		req.prompt = "hello";
		req.parent = "parent";
		req.provider = "mock";
		req.model = "mock-model";
		req.tool_filter = araya::tools::tool_restriction{.deny = std::vector<std::string>{"ghost"}};
		auto result = co_await r.subs->run("spawn", std::move(req));
		CHECK(result.is_error);
		CHECK(result.error.find("unknown tool") != std::string::npos);
	});
}
