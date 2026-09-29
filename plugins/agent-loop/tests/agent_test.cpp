#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "support/plugin_harness.hpp"
#include "support/stream_chunks.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace araya::agent;
using namespace araya::llm;
using namespace araya::llm_bridge;
using namespace araya_test::llm;
using namespace std::chrono_literals;

// -- the scripted adapter ---------------------------------------------------

struct scripted_call {
	std::vector<stream_chunk> chunks;
	bool wait_for_stop = false;
};

struct scripted_adapter : llm_adapter {
	std::vector<scripted_call> script;
	mutable std::size_t calls = 0;
	mutable std::vector<generate_options> seen;

	araya::task<void> stream(generate_options const& options, chunk_sink const& sink) override {
		seen.push_back(options);
		auto index = calls++;
		if (index >= script.size())
			co_return;
		auto const& call = script[index];
		for (auto const& chunk : call.chunks)
			co_await sink(chunk);
		if (call.wait_for_stop) {
			auto ex = co_await boost::asio::this_coro::executor;
			boost::asio::steady_timer poll(ex, 1ms);
			while (!options.stop_token.stop_requested()) {
				poll.expires_after(1ms);
				co_await poll.async_wait(boost::asio::use_awaitable);
			}
			co_await sink(finish_chunk{
				finish_chunk::reason::aborted, llm_failure{llm_error_code::aborted, "cancelled"}, std::nullopt});
		}
	}
};

std::shared_ptr<scripted_adapter> g_scripted;

struct adapter_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto service = ctx.require<llm_service>(llm_key);
		registration = service->register_adapter({"mock"}, g_scripted, ctx);
		co_return;
	}

	araya::registration registration;
};

std::unique_ptr<araya::plugin> make_adapter(araya::plugin_config const&) { return std::make_unique<adapter_plugin>(); }

static const araya::dependency_spec g_llm_dep[]{{araya::service_id{"llm", 1}, true, {}}};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::plugin_descriptor g_adapter_desc{"adapter", g_llm_dep, g_no_provs, &make_adapter};

// -- the harness ------------------------------------------------------------

struct harness : araya_test::plugin_harness {
	template <typename Fn>
	void run(Fn&& fn) {
		g_scripted.reset();
		plugin_harness::run(std::forward<Fn>(fn));
	}

	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }

	araya::component_spec llm_spec() { return spec(&araya::llm::plugin_descriptor()); }

	araya::component_spec adapter_spec() { return spec(&g_adapter_desc); }

	// The harness identity is off by default so an agent test without a
	// persona renders an empty prompt (no system message).
	araya::component_spec system_prompt_spec(araya::plugin_config cfg = {{"include_harness_identity", "false"}}) {
		return spec(&araya::system_prompt::plugin_descriptor(), std::move(cfg));
	}

	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }

	araya::component_spec agent_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::agent::plugin_descriptor(), std::move(cfg));
	}
};

// Mounts session + llm + the scripted adapter + the prompt registry + the
// tool registry + the agent, and returns a ready session plus the
// services.
struct rig {
	harness& h;
	std::shared_ptr<araya::session::session> session;
	std::shared_ptr<agent_service> agent;
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts;
	std::shared_ptr<araya::tools::tools_service> tools;

	araya::task<void> mount(
		araya::runtime& rt,
		araya::plugin_config agent_config = {},
		araya::plugin_config prompt_config = {{"include_harness_identity", "false"}}) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.adapter_spec());
		co_await rt.mount(h.system_prompt_spec(std::move(prompt_config)));
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec(std::move(agent_config)));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto store = root_ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		session = store->create(root_ctx, araya::session::session_id{"s1"}, {});
		agent = root_ctx.require<agent_service>(agent_key).shared();
		prompts = root_ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key)
					  .shared();
		tools = root_ctx.require<araya::tools::tools_service>(araya::tools::tools_key).shared();
	}
};

// -- log/surface inspection helpers -----------------------------------------

std::vector<std::string> event_types(araya::session::session const& s) {
	std::vector<std::string> types;
	for (auto const& event : s.log())
		types.push_back(event.type);
	return types;
}

std::size_t count_type(araya::session::session const& s, std::string_view type) {
	std::size_t count = 0;
	for (auto const& event : s.log()) {
		if (event.type == type)
			++count;
	}
	return count;
}

boost::json::value const*
find_data(araya::session::session const& s, std::string_view type, std::size_t occurrence = 0) {
	std::size_t seen = 0;
	for (auto const& event : s.log()) {
		if (event.type != type)
			continue;
		if (seen++ == occurrence)
			return &event.data;
	}
	return nullptr;
}

std::vector<std::string> surface_roles(araya::session::session const& s) {
	std::vector<std::string> roles;
	for (auto const& message : s.surface().messages()) {
		switch (message.role) {
		case araya::session::message_role::system:
			roles.emplace_back("system");
			break;
		case araya::session::message_role::user:
			roles.emplace_back("user");
			break;
		case araya::session::message_role::assistant:
			roles.emplace_back("assistant");
			break;
		case araya::session::message_role::tool_result:
			roles.emplace_back("tool");
			break;
		}
	}
	return roles;
}

run_options options_for(std::string input = "hello") {
	run_options options;
	options.provider = "mock";
	options.model = "test-model";
	options.session = araya::session::session_id{"s1"};
	options.input = std::move(input);
	return options;
}

araya::task<araya::tools::tool_result> echo_tool(araya::tools::tool_context const& ctx) {
	std::string text = "ok";
	if (auto const* object = ctx.arguments.if_object()) {
		if (auto const* x = object->if_contains("x"))
			text += " " + std::to_string(x->as_int64());
	}
	co_return araya::tools::tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, false};
}

} // namespace

TEST_CASE("a text-only turn commits the full event envelope") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("hello")}};
		rig r{h};
		co_await r.mount(rt);

		std::vector<agent_event> events;
		event_sink sink = [&](agent_event const& event) { events.push_back(event); };

		auto outcome = co_await r.agent->run(options_for("hi"), sink);

		CHECK(outcome.status == run_status::completed);
		CHECK(outcome.turn == 1);
		CHECK(
			event_types(*r.session) == std::vector<std::string>{
										   "turn/start",
										   "step/start",
										   "user/message",
										   "request/header",
										   "request/context",
										   "assistant/message",
										   "step/end",
										   "turn/end"});
		CHECK(surface_roles(*r.session) == std::vector<std::string>{"user", "assistant"});
		CHECK(find_data(*r.session, "turn/end")->at("reason") == "completed");
		CHECK(find_data(*r.session, "request/header")->at("header").at("model") == "test-model");
		CHECK(find_data(*r.session, "assistant/message")->at("usage").at("output_tokens") == 2);
		CHECK(std::holds_alternative<turn_event>(events[0]));
		CHECK(std::holds_alternative<step_event>(events[1]));
		CHECK(std::holds_alternative<run_finish_event>(events.back()));
		CHECK(std::get<run_finish_event>(events.back()).status == run_status::completed);
		// the user text reached the adapter
		REQUIRE(g_scripted->seen.size() == 1);
		CHECK(g_scripted->seen[0].session_id == "s1");
		CHECK(g_scripted->seen[0].provider == "mock");
	});
}

TEST_CASE("tool calls execute and feed the next step") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{tool_stream("echo", "{\"x\": 1}")}, {text_stream("done")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		r.tools->register_tool(
			root_ctx,
			araya::tools::tool_definition{"echo", "echoes x", boost::json::value{{"type", "object"}}},
			&echo_tool);

		std::vector<agent_event> events;
		event_sink sink = [&](agent_event const& event) { events.push_back(event); };

		auto outcome = co_await r.agent->run(options_for("hi"), sink);

		CHECK(outcome.status == run_status::completed);
		CHECK(count_type(*r.session, "step/start") == 2);
		CHECK(count_type(*r.session, "assistant/message") == 2);
		CHECK(count_type(*r.session, "tool/call") == 1);
		CHECK(count_type(*r.session, "tool/result") == 1);
		// the request header does not repeat when nothing changed
		CHECK(count_type(*r.session, "request/header") == 1);
		CHECK(surface_roles(*r.session) == std::vector<std::string>{"user", "assistant", "tool", "assistant"});

		auto const* result = find_data(*r.session, "tool/result");
		CHECK(result->at("content").as_array().size() == 1);
		auto const& block = result->at("content").as_array()[0];
		CHECK(block.at("tool_call_id") == "call-1");
		CHECK(block.at("is_error") == false);
		CHECK(result->at("source").at("call_id") == "call-1");

		// the second step's history carries the tool result
		REQUIRE(g_scripted->seen.size() == 2);
		auto const& second = g_scripted->seen[1];
		CHECK(second.tools.size() == 1);
		CHECK(second.tools[0].name == "echo");
		REQUIRE(second.messages.size() == 3);
		CHECK(second.messages[2].role == message_role::user);
		REQUIRE(second.messages[2].content.size() == 1);
		auto const* tool = std::get_if<tool_result_block>(&second.messages[2].content[0]);
		REQUIRE(tool != nullptr);
		CHECK(tool->tool_call_id == "call-1");
		CHECK(tool->is_error == false);

		bool saw_tool_event = false;
		bool saw_tool_done = false;
		for (auto const& event : events) {
			if (auto const* call = std::get_if<tool_call_event>(&event)) {
				saw_tool_event = true;
				CHECK(call->name == "echo");
			}
			if (auto const* done = std::get_if<tool_done_event>(&event)) {
				saw_tool_done = true;
				CHECK(done->name == "echo");
				CHECK(done->is_error == false);
			}
		}
		CHECK(saw_tool_event);
		CHECK(saw_tool_done);
	});
}

TEST_CASE("unknown tools record error results and continue") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{tool_stream("ghost", "{}")}, {text_stream("still here")}};
		rig r{h};
		co_await r.mount(rt);

		auto outcome = co_await r.agent->run(options_for("go"), {});

		CHECK(outcome.status == run_status::completed);
		CHECK(count_type(*r.session, "tool/result") == 1);
		CHECK(count_type(*r.session, "assistant/message") == 2);
		auto const* result = find_data(*r.session, "tool/result");
		auto const& block = result->at("content").as_array()[0];
		CHECK(block.at("is_error") == true);
		CHECK(block.at("content").as_array()[0].at("text").as_string() == "Error: unknown tool 'ghost'");
		CHECK(surface_roles(*r.session) == std::vector<std::string>{"user", "assistant", "tool", "assistant"});
	});
}

TEST_CASE("throwing handlers record error results and continue") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{tool_stream("boom", "{}")}, {text_stream("survived")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		r.tools->register_tool(
			root_ctx,
			araya::tools::tool_definition{"boom", "explodes", boost::json::value(nullptr)},
			[](araya::tools::tool_context const&) -> araya::task<araya::tools::tool_result> {
				throw std::runtime_error("kaboom");
			});

		auto outcome = co_await r.agent->run(options_for("go"), {});

		CHECK(outcome.status == run_status::completed);
		auto const* result = find_data(*r.session, "tool/result");
		auto const& block = result->at("content").as_array()[0];
		CHECK(block.at("is_error") == true);
		CHECK(block.at("content").as_array()[0].at("text").as_string() == "Error: kaboom");
	});
}

TEST_CASE("abort finalizes delivered text as an interrupted assistant message") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {
			{{stream_chunk{block_start_chunk{0, content_block_type::text}},
			  stream_chunk{text_delta_chunk{0, "partial "}}},
			 true}};
		rig r{h};
		co_await r.mount(rt);

		std::stop_source source;
		auto ex = co_await boost::asio::this_coro::executor;
		boost::asio::steady_timer timer(ex, 5ms);
		timer.async_wait([&](boost::system::error_code) { source.request_stop(); });

		auto options = options_for("stop me");
		options.stop = source.get_token();
		auto outcome = co_await r.agent->run(options, {});

		CHECK(outcome.status == run_status::aborted);
		CHECK(count_type(*r.session, "assistant/message") == 1);
		CHECK(count_type(*r.session, "assistant/attempt") == 0);
		auto const* message = find_data(*r.session, "assistant/message");
		CHECK(message->at("interrupted") == true);
		CHECK(message->at("message").at("content").as_array()[0].at("text").as_string() == "partial ");
		CHECK(find_data(*r.session, "turn/end")->at("reason") == "aborted");
		CHECK(surface_roles(*r.session) == std::vector<std::string>{"user", "assistant"});
	});
}

TEST_CASE("a provider failure is an attempt, not a message") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{error_stream("provider exploded")}};
		rig r{h};
		co_await r.mount(rt);

		auto outcome = co_await r.agent->run(options_for("go"), {});

		CHECK(outcome.status == run_status::error);
		REQUIRE(outcome.failure.has_value());
		CHECK(outcome.failure->code == llm_error_code::server);
		CHECK(count_type(*r.session, "assistant/message") == 0);
		CHECK(count_type(*r.session, "assistant/attempt") == 1);
		CHECK(find_data(*r.session, "turn/end")->at("reason") == "error");
		CHECK(surface_roles(*r.session) == std::vector<std::string>{"user"});
	});
}

TEST_CASE("turns number consecutively and the step cap blocks a tool loop") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("first")}, {text_stream("second")}};
		rig r{h};
		co_await r.mount(rt);

		auto first = co_await r.agent->run(options_for("first"), {});
		CHECK(first.turn == 1);
		auto second = co_await r.agent->run(options_for("again"), {});
		CHECK(second.turn == 2);
		CHECK(second.status == run_status::completed);
		CHECK(count_type(*r.session, "turn/start") == 2);
		CHECK(count_type(*r.session, "turn/end") == 2);
	});

	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{tool_stream("echo", "{}")}, {text_stream("never")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		r.tools->register_tool(
			root_ctx, araya::tools::tool_definition{"echo", "echoes", boost::json::value(nullptr)}, &echo_tool);

		auto options = options_for("go");
		options.max_steps = 1;
		auto outcome = co_await r.agent->run(options, {});

		CHECK(outcome.status == run_status::blocked);
		CHECK(count_type(*r.session, "step/start") == 1);
		CHECK(count_type(*r.session, "assistant/message") == 1);
		CHECK(count_type(*r.session, "tool/result") == 1);
		CHECK(find_data(*r.session, "turn/end")->at("reason") == "blocked");
		CHECK(g_scripted->calls == 1);
	});
}

TEST_CASE("the prompt registry commits one effective system node") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("hi")}, {text_stream("hi again")}, {text_stream("hi a third")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		r.prompts->set_persona_prefix("be helpful");
		r.prompts->section(root_ctx, {"tests", 1000, "extra"});

		auto first = co_await r.agent->run(options_for("first"), {});
		CHECK(first.status == run_status::completed);
		CHECK(count_type(*r.session, "system/message") == 1);
		CHECK(surface_roles(*r.session) == std::vector<std::string>{"system", "user", "assistant"});
		CHECK(find_data(*r.session, "system/message")->at("message").at("source").at("plugin") == "agent-loop");

		// An unchanged prompt adds no node on the next turn.
		co_await r.agent->run(options_for("two"), {});
		CHECK(count_type(*r.session, "system/message") == 1);
		CHECK(count_type(*r.session, "surface/replace") == 0);

		// A changed prompt appends a new in-history system node.
		r.prompts->set_persona_prefix("be terse");
		co_await r.agent->run(options_for("three"), {});
		CHECK(count_type(*r.session, "system/message") == 2);
		CHECK(count_type(*r.session, "surface/replace") == 0);
		CHECK(surface_roles(*r.session).front() == "system");
	});
}

namespace {

boost::json::value inbox_entry(std::string id, std::string text = "queued") {
	return boost::json::object{
		{"id", std::move(id)},
		{"role", "user"},
		{"content", boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}}};
}

} // namespace

TEST_CASE("the inbox fold applies toSpliced splices") {
	inbox_state state;
	apply_inbox_splice(state, inbox_append_data(inbox_target::next_turn, inbox_entry("a")));
	apply_inbox_splice(state, inbox_append_data(inbox_target::next_turn, inbox_entry("b")));
	apply_inbox_splice(state, inbox_prepend_data(inbox_target::next_step, inbox_entry("s1")));
	apply_inbox_splice(state, inbox_prepend_data(inbox_target::next_step, inbox_entry("s0")));

	REQUIRE(state.next_turn.size() == 2);
	CHECK(inbox_message_id(state.next_turn[0]) == "a");
	CHECK(inbox_message_id(state.next_turn[1]) == "b");
	REQUIRE(state.next_step.size() == 2);
	CHECK(inbox_message_id(state.next_step[0]) == "s0");
	CHECK(inbox_message_id(state.next_step[1]) == "s1");

	// Message ids are unique across both queues: a duplicate is dropped.
	apply_inbox_splice(state, inbox_append_data(inbox_target::next_step, inbox_entry("a")));
	CHECK(state.next_step.size() == 2);

	// A claim removes from the front.
	apply_inbox_splice(state, inbox_claim_data(inbox_target::next_turn, 1));
	REQUIRE(state.next_turn.size() == 1);
	CHECK(inbox_message_id(state.next_turn[0]) == "b");

	// A replace splices a removal and an insertion at one index.
	apply_inbox_splice(state, inbox_splice_data(inbox_target::next_step, 0, 1, boost::json::array{inbox_entry("s2")}));
	REQUIRE(state.next_step.size() == 2);
	CHECK(inbox_message_id(state.next_step[0]) == "s2");
	CHECK(inbox_message_id(state.next_step[1]) == "s1");
}

TEST_CASE("the inbox fold clamps coordinates and ignores malformed payloads") {
	inbox_state state;
	apply_inbox_splice(state, inbox_append_data(inbox_target::next_turn, inbox_entry("a")));
	apply_inbox_splice(state, inbox_append_data(inbox_target::next_turn, inbox_entry("b")));

	// A negative start counts from the end.
	apply_inbox_splice(state, inbox_splice_data(inbox_target::next_turn, -1, 1));
	REQUIRE(state.next_turn.size() == 1);
	CHECK(inbox_message_id(state.next_turn[0]) == "a");

	// Malformed payloads are no-ops.
	apply_inbox_splice(state, boost::json::value{7});
	apply_inbox_splice(state, boost::json::value(boost::json::object{{"target", "next-turn"}}));
	apply_inbox_splice(state, boost::json::value(boost::json::object{{"target", "elsewhere"}, {"start", 0}}));
	CHECK(state.next_turn.size() == 1);

	// An out-of-range removal clamps to the end.
	apply_inbox_splice(state, inbox_claim_data(inbox_target::next_turn, 99));
	CHECK(state.next_turn.empty());
}

TEST_CASE("the agent inbox projection folds appends and replay") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("hi")}};
		rig r{h};
		co_await r.mount(rt);

		auto id = r.session->id();
		auto const* inbox = r.agent->inbox(id);
		REQUIRE(inbox != nullptr);
		CHECK(inbox->next_turn.empty());
		CHECK(inbox->next_step.empty());

		r.session->append(
			"agent/inbox/spliced", inbox_append_data(inbox_target::next_turn, inbox_entry("n1", "knock")));
		r.session->append(
			"agent/inbox/spliced", inbox_prepend_data(inbox_target::next_step, inbox_entry("s1", "steer")));

		inbox = r.agent->inbox(id);
		REQUIRE(inbox != nullptr);
		REQUIRE(inbox->next_turn.size() == 1);
		CHECK(inbox_message_id(inbox->next_turn[0]) == "n1");
		REQUIRE(inbox->next_step.size() == 1);
		CHECK(inbox_message_id(inbox->next_step[0]) == "s1");

		// The inbox is durable: a restore replays it into a fresh cell
		// alongside the surface.
		auto store = rt.root_context().require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto log = r.session->log();
		store->dispose(id);
		CHECK(r.agent->inbox(id) == nullptr);

		auto restored = store->prepare(
			id, araya::session::create_session_options{.seed = log, .inherited_event_count = log.size()});
		store->enter(restored);
		store->announce(*restored);

		auto const* replayed = r.agent->inbox(id);
		REQUIRE(replayed != nullptr);
		REQUIRE(replayed->next_turn.size() == 1);
		CHECK(inbox_message_id(replayed->next_turn[0]) == "n1");
		REQUIRE(replayed->next_step.size() == 1);
		CHECK(inbox_message_id(replayed->next_step[0]) == "s1");
	});
}

namespace {

drive_options mock_exec() {
	drive_options exec;
	exec.provider = "mock";
	exec.model = "test-model";
	return exec;
}

boost::json::value tagged(std::string id, std::string text, std::string kind = "user") {
	return user_message_data(std::move(id), std::move(text), message_source(std::move(kind)));
}

template <class Pred>
araya::task<void> spin_until(Pred pred) {
	auto ex = co_await boost::asio::this_coro::executor;
	boost::asio::steady_timer timer(ex, 1ms);
	while (!pred()) {
		timer.expires_after(1ms);
		co_await timer.async_wait(boost::asio::use_awaitable);
	}
}

std::vector<std::string> user_texts(araya::session::session const& s) {
	std::vector<std::string> out;
	for (auto const& message : s.surface().messages()) {
		if (message.role == araya::session::message_role::user)
			out.push_back(araya::llm_bridge::message_text(message));
	}
	return out;
}

} // namespace

TEST_CASE("a session acquires and loses an agent driver with its lifecycle") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();

		int created = 0;
		int disposed = 0;
		root_ctx.on(agent_created_key, [&](agent_created_msg const&) { ++created; });
		root_ctx.on(agent_disposed_key, [&](agent_disposed_msg const&) { ++disposed; });

		auto store = root_ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		store->create(root_ctx, araya::session::session_id{"s2"});
		co_await rt.wait_idle();
		CHECK(created == 1);
		CHECK(r.agent->status(araya::session::session_id{"s2"}) == agent_status::idle);
		CHECK(r.agent->scope_of(araya::session::session_id{"s2"}) != nullptr);

		store->dispose(araya::session::session_id{"s2"});
		co_await rt.wait_idle();
		CHECK(disposed == 1);
		CHECK(r.agent->scope_of(araya::session::session_id{"s2"}) == nullptr);
	});
}

TEST_CASE("a followup wakes the driver, claims its message, and reports status") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("answer")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		auto id = r.session->id();

		std::vector<std::string> statuses;
		std::vector<std::string> inserted;
		std::vector<std::string> claimed;
		root_ctx.on(agent_status_key, [&](agent_status_msg const& m) {
			statuses.push_back(m.status == agent_status::running ? "running" : "idle");
		});
		root_ctx.on(agent_inbox_inserted_key, [&](agent_inbox_inserted_msg const& m) {
			inserted.push_back(inbox_message_id(m.message));
		});
		root_ctx.on(agent_inbox_claimed_key, [&](agent_inbox_claimed_msg const& m) {
			claimed.push_back(inbox_message_id(m.message));
		});

		co_await r.agent->followup(id, tagged("n1", "queued"), mock_exec());
		co_await r.agent->when_idle(id);

		CHECK(statuses == std::vector<std::string>{"running", "idle"});
		CHECK(inserted == std::vector<std::string>{"n1"});
		CHECK(claimed == std::vector<std::string>{"n1"});
		CHECK(r.agent->status(id) == agent_status::idle);
		CHECK(r.agent->last_outcome(id).status == run_status::completed);
		CHECK(user_texts(*r.session) == std::vector<std::string>{"queued"});

		auto const* boundary = r.agent->turn_boundary(id);
		REQUIRE(boundary != nullptr);
		CHECK(boundary->last_turn == 1);
		CHECK(!boundary->open_turn_start_seq.has_value());
		REQUIRE(boundary->last_step_boundary.has_value());
		CHECK(boundary->last_step_boundary->start == false);
	});
}

TEST_CASE("a turn claims next-step input before its next-turn message") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("done")}};
		rig r{h};
		co_await r.mount(rt);
		auto id = r.session->id();

		co_await r.agent->inject(id, tagged("s1", "steer", "plugin"));
		co_await r.agent->followup(id, tagged("n1", "open"), mock_exec());
		co_await r.agent->when_idle(id);

		CHECK(user_texts(*r.session) == std::vector<std::string>{"steer", "open"});
	});
}

TEST_CASE("inject queues without waking a driver") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		rig r{h};
		co_await r.mount(rt);
		auto id = r.session->id();

		co_await r.agent->inject(id, tagged("s1", "later", "plugin"));

		CHECK(r.agent->status(id) == agent_status::idle);
		CHECK(count_type(*r.session, "turn/start") == 0);
		auto const* inbox = r.agent->inbox(id);
		REQUIRE(inbox != nullptr);
		REQUIRE(inbox->next_step.size() == 1);
		CHECK(inbox_message_id(inbox->next_step[0]) == "s1");
	});
}

TEST_CASE("the pre-step waterfall can reject a step") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("never")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		auto id = r.session->id();

		root_ctx.on(pre_step_key, [](pre_step_msg msg, araya::waterfall_continuation<pre_step_msg>) -> pre_step_msg {
			msg.reject = true;
			return msg;
		});

		co_await r.agent->followup(id, tagged("n1", "go"), mock_exec());
		co_await r.agent->when_idle(id);

		CHECK(r.agent->last_outcome(id).status == run_status::blocked);
		CHECK(count_type(*r.session, "step/start") == 0);
		CHECK(count_type(*r.session, "assistant/message") == 0);
		CHECK(count_type(*r.session, "turn/end") == 1);
		CHECK(find_data(*r.session, "turn/end")->at("reason") == "blocked");
		// The claimed (then rejected) message never entered the surface.
		CHECK(user_texts(*r.session).empty());
		CHECK(g_scripted->calls == 0);
	});
}

TEST_CASE("the pre-step waterfall can add messages to a step") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("done")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		auto id = r.session->id();

		root_ctx.on(pre_step_key, [](pre_step_msg msg, araya::waterfall_continuation<pre_step_msg>) -> pre_step_msg {
			msg.messages.push_back(user_message_data("injected", "from listener", message_source("plugin")));
			return msg;
		});

		co_await r.agent->followup(id, tagged("n1", "go"), mock_exec());
		co_await r.agent->when_idle(id);

		CHECK(user_texts(*r.session) == std::vector<std::string>{"go", "from listener"});
	});
}

TEST_CASE("a route-less followup inherits the session's logged route") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("first")}, {text_stream("second")}};
		rig r{h};
		co_await r.mount(rt);
		auto id = r.session->id();

		auto first = co_await r.agent->run(options_for("one"), {});
		CHECK(first.status == run_status::completed);

		// Drop the driver so its in-memory route is gone; the next followup
		// must recover the route from the logged request header.
		co_await r.agent->dispose(id);
		co_await r.agent->followup(id, tagged("n2", "two"), {});
		co_await r.agent->when_idle(id);

		CHECK(count_type(*r.session, "turn/start") == 2);
		CHECK(r.agent->last_outcome(id).status == run_status::completed);
	});
}

TEST_CASE("cancel stops the drive and keep_inbox preserves pending input") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {
			{{stream_chunk{block_start_chunk{0, content_block_type::text}},
			  stream_chunk{text_delta_chunk{0, "partial "}}},
			 true}};
		rig r{h};
		co_await r.mount(rt);
		auto id = r.session->id();

		co_await r.agent->followup(id, tagged("n1", "start"), mock_exec());
		co_await spin_until([&] { return r.agent->status(id) == agent_status::running; });
		co_await r.agent->inject(id, tagged("s1", "pending", "plugin"));
		r.agent->cancel(id, cancel_cause::user, /*keep_inbox=*/true);
		co_await r.agent->when_idle(id);

		CHECK(r.agent->status(id) == agent_status::idle);
		CHECK(r.agent->last_outcome(id).status == run_status::aborted);
		auto const* inbox = r.agent->inbox(id);
		REQUIRE(inbox != nullptr);
		CHECK(inbox->next_turn.empty());
		CHECK(inbox->next_step.size() == 1);
	});

	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {
			{{stream_chunk{block_start_chunk{0, content_block_type::text}},
			  stream_chunk{text_delta_chunk{0, "partial "}}},
			 true}};
		rig r{h};
		co_await r.mount(rt);
		auto id = r.session->id();

		co_await r.agent->followup(id, tagged("n1", "start"), mock_exec());
		co_await spin_until([&] { return r.agent->status(id) == agent_status::running; });
		co_await r.agent->inject(id, tagged("s1", "pending", "plugin"));
		r.agent->cancel(id, cancel_cause::user, /*keep_inbox=*/false);
		co_await r.agent->when_idle(id);

		auto const* inbox = r.agent->inbox(id);
		REQUIRE(inbox != nullptr);
		CHECK(inbox->next_turn.empty());
		CHECK(inbox->next_step.empty());
	});
}

TEST_CASE("the turn-boundary fold replays with the session log") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("hi")}};
		rig r{h};
		co_await r.mount(rt);
		auto id = r.session->id();

		co_await r.agent->followup(id, tagged("n1", "go"), mock_exec());
		co_await r.agent->when_idle(id);
		REQUIRE(r.agent->turn_boundary(id) != nullptr);
		CHECK(r.agent->turn_boundary(id)->last_turn == 1);

		auto store = rt.root_context().require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto log = r.session->log();
		store->dispose(id);
		CHECK(r.agent->turn_boundary(id) == nullptr);

		auto restored = store->prepare(
			id, araya::session::create_session_options{.seed = log, .inherited_event_count = log.size()});
		store->enter(restored);
		store->announce(*restored);
		auto const* boundary = r.agent->turn_boundary(id);
		REQUIRE(boundary != nullptr);
		CHECK(boundary->last_turn == 1);
		CHECK(!boundary->open_turn_start_seq.has_value());
	});
}

TEST_CASE("agent events are scoped to their session's realm") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("a")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		auto store = root_ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto other = store->create(root_ctx, araya::session::session_id{"s2"});
		co_await rt.wait_idle();

		int s1_running = 0;
		int s2_running = 0;
		root_ctx.on(
			agent_status_key,
			[&](agent_status_msg const& m) {
				if (m.status == agent_status::running)
					++s1_running;
			},
			{.scope = r.agent->scope_of(r.session->id())});
		root_ctx.on(
			agent_status_key,
			[&](agent_status_msg const& m) {
				if (m.status == agent_status::running)
					++s2_running;
			},
			{.scope = r.agent->scope_of(other->id())});

		co_await r.agent->followup(r.session->id(), tagged("n1", "go"), mock_exec());
		co_await r.agent->when_idle(r.session->id());

		CHECK(s1_running == 1);
		CHECK(s2_running == 0);
	});
}

TEST_CASE("context producers contribute ordered, idempotent messages before the claim") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("a")}, {text_stream("b")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		auto id = r.session->id();

		r.agent->add_context_producer(
			root_ctx, context_producer{"alpha", [](araya::session::session const& s, context_producer_context const&) {
										   return injected_context(s, "alpha", "alpha-body");
									   }});
		r.agent->add_context_producer(
			root_ctx, context_producer{"beta", [](araya::session::session const& s, context_producer_context const&) {
										   return injected_context(s, "beta", "beta-body");
									   }});

		co_await r.agent->followup(id, tagged("n1", "first"), mock_exec());
		co_await r.agent->when_idle(id);
		co_await r.agent->followup(id, tagged("n2", "second"), mock_exec());
		co_await r.agent->when_idle(id);

		// Registration order, before the claimed message; the second turn
		// re-injects nothing because both renders are unchanged.
		CHECK(user_texts(*r.session) == std::vector<std::string>{"alpha-body", "beta-body", "first", "second"});
	});
}

TEST_CASE("a changed context message is re-injected and survives restore") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("a")}, {text_stream("b")}, {text_stream("c")}};
		rig r{h};
		co_await r.mount(rt);
		auto root_ctx = rt.root_context();
		auto id = r.session->id();

		std::string body = "one";
		r.agent->add_context_producer(
			root_ctx,
			context_producer{"changing", [&body](araya::session::session const& s, context_producer_context const&) {
								 return injected_context(s, "changing", body);
							 }});

		co_await r.agent->followup(id, tagged("n1", "first"), mock_exec());
		co_await r.agent->when_idle(id);
		body = "two";
		co_await r.agent->followup(id, tagged("n2", "second"), mock_exec());
		co_await r.agent->when_idle(id);
		CHECK(user_texts(*r.session) == std::vector<std::string>{"one", "first", "two", "second"});

		// The injected context is durable: a restore replays it, and an
		// unchanged render contributes nothing more.
		auto store = root_ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto log = r.session->log();
		store->dispose(id);
		auto restored = store->prepare(
			id, araya::session::create_session_options{.seed = log, .inherited_event_count = log.size()});
		store->enter(restored);
		store->announce(*restored);
		co_await r.agent->followup(id, tagged("n3", "third"), mock_exec());
		co_await r.agent->when_idle(id);
		CHECK(user_texts(*restored) == std::vector<std::string>{"one", "first", "two", "second", "third"});
	});
}
