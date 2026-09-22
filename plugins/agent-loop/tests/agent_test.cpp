#include <catch2/catch_test_macros.hpp>

#include "araya/agent-loop/agent.hpp"
#include "araya/agent-loop/bridge.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"

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

// -- chunk scripts ----------------------------------------------------------

std::vector<stream_chunk> text_stream(std::string text) {
	token_usage usage;
	usage.input_tokens = 3;
	usage.output_tokens = 2;
	return {
		stream_chunk{block_start_chunk{0, content_block_type::text}},
		stream_chunk{text_delta_chunk{0, text}},
		stream_chunk{block_end_chunk{0, content_block{text_block{text}}}},
		stream_chunk{usage_chunk{usage}},
		stream_chunk{finish_chunk{finish_chunk::reason::stop, std::nullopt, std::nullopt}},
	};
}

std::vector<stream_chunk> tool_stream(std::string name, std::string arguments) {
	tool_call_block call{"call-1", name, arguments};
	token_usage usage;
	usage.input_tokens = 4;
	usage.output_tokens = 1;
	return {
		stream_chunk{block_start_chunk{0, content_block_type::tool_call}},
		stream_chunk{tool_call_delta_chunk{0, "call-1", name, arguments}},
		stream_chunk{block_end_chunk{0, content_block{call}}},
		stream_chunk{usage_chunk{usage}},
		stream_chunk{finish_chunk{finish_chunk::reason::tool_calls, std::nullopt, std::nullopt}},
	};
}

std::vector<stream_chunk> error_stream(std::string message) {
	llm_failure failure{llm_error_code::server, std::move(message)};
	return {
		stream_chunk{finish_chunk{finish_chunk::reason::error, std::move(failure), std::nullopt}},
	};
}

// -- the harness ------------------------------------------------------------

struct harness {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	template <typename Fn>
	void run(Fn&& fn) {
		g_scripted.reset();
		struct driver {
			std::decay_t<Fn> fn;
			harness* self;
			araya::task<void> operator()() { co_await fn(*self->rt); }
		};
		boost::asio::co_spawn(io.get_executor(), driver{std::forward<Fn>(fn), this}, boost::asio::detached);
		io.run();
		io.restart();
	}

	araya::component_spec spec(araya::plugin_descriptor const* d, araya::plugin_config cfg = {}) {
		return araya::component_spec{
			std::shared_ptr<araya::plugin_descriptor>(const_cast<araya::plugin_descriptor*>(d), [](auto*) {}),
			std::move(cfg),
			nullptr,
			""};
	}

	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }

	araya::component_spec llm_spec() { return spec(&araya::llm::plugin_descriptor()); }

	araya::component_spec adapter_spec() { return spec(&g_adapter_desc); }

	araya::component_spec agent_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::agent::plugin_descriptor(), std::move(cfg));
	}
};

// Mounts session + llm + the scripted adapter + the agent, and returns a
// ready session plus the services.
struct rig {
	harness& h;
	std::shared_ptr<araya::session::session> session;
	std::shared_ptr<agent_service> agent;

	araya::task<void> mount(araya::runtime& rt, araya::plugin_config agent_config = {}) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.adapter_spec());
		co_await rt.mount(h.agent_spec(std::move(agent_config)));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto store = root_ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		session = store->create(root_ctx, araya::session::session_id{"s1"}, {});
		agent = root_ctx.require<agent_service>(agent_key).shared();
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

araya::task<tool_result> echo_tool(tool_context const& ctx) {
	std::string text = "ok";
	if (auto const* object = ctx.arguments.if_object()) {
		if (auto const* x = object->if_contains("x"))
			text += " " + std::to_string(x->as_int64());
	}
	co_return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, false};
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
		r.agent->register_tool(
			root_ctx, tool_spec{"echo", "echoes x", boost::json::value{{"type", "object"}}}, &echo_tool);

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
		r.agent->register_tool(
			root_ctx,
			tool_spec{"boom", "explodes", boost::json::value(nullptr)},
			[](tool_context const&) -> araya::task<tool_result> { throw std::runtime_error("kaboom"); });

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
		r.agent->register_tool(root_ctx, tool_spec{"echo", "echoes", boost::json::value(nullptr)}, &echo_tool);

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

TEST_CASE("the configured system prompt commits once; registered sections stack") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_scripted = std::make_shared<scripted_adapter>();
		g_scripted->script = {{text_stream("hi")}, {text_stream("hi again")}};
		rig r{h};
		co_await r.mount(rt, {{"system_prompt", "be helpful"}});
		auto root_ctx = rt.root_context();
		r.agent->register_system_prompt(root_ctx, "tests", [] { return std::string("extra"); });

		CHECK(r.agent->system_prompt() == "be helpful");
		auto first = co_await r.agent->run(options_for("first"), {});
		CHECK(first.status == run_status::completed);
		CHECK(count_type(*r.session, "system/message") == 2);
		CHECK(surface_roles(*r.session) == std::vector<std::string>{"system", "system", "user", "assistant"});
		CHECK(find_data(*r.session, "system/message", 0)->at("message").at("source").at("plugin") == "agent-loop");
		CHECK(find_data(*r.session, "system/message", 1)->at("message").at("source").at("plugin") == "tests");

		// a second turn does not duplicate the prompts
		co_await r.agent->run(options_for("two"), {});
		CHECK(count_type(*r.session, "system/message") == 2);
	});
}
