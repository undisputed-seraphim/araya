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
#include "araya/workflow/workflow.hpp"
#include "protocol.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <cstdlib>
#include <memory>
#include <string>

namespace {

using namespace araya::workflow;

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
	araya::component_spec workflow_spec_missing_node() {
		return spec(&araya::workflow::plugin_descriptor(), {{"node_executable", "/nonexistent/araya-node-not-here"}});
	}
};

bool node_available() { return std::system("command -v node >/dev/null 2>&1") == 0; }

} // namespace

TEST_CASE("control frames round-trip") {
	boost::json::value message{{"type", "call"}, {"id", 7}, {"args", boost::json::object{{"prompt", "hi"}}}};
	auto const frame = encode_frame(message);
	REQUIRE(frame.size() > 4);
	auto const length = (static_cast<unsigned char>(frame[0]) << 24) | (static_cast<unsigned char>(frame[1]) << 16) |
						(static_cast<unsigned char>(frame[2]) << 8) | static_cast<unsigned char>(frame[3]);
	CHECK(length == frame.size() - 4);

	frame_decoder decoder(1024);
	auto messages = decoder.feed(frame);
	REQUIRE(messages.size() == 1);
	CHECK(messages[0] == message);
}

TEST_CASE("the decoder reassembles split frames and batches coalesced ones") {
	auto const one = encode_frame(boost::json::value{{"type", "log"}, {"text", "a"}});
	auto const two = encode_frame(boost::json::value{{"type", "log"}, {"text", "b"}});
	auto const both = one + two;
	frame_decoder decoder(1024);
	CHECK(decoder.feed(std::string_view(both).substr(0, 3)).empty());
	auto messages = decoder.feed(std::string_view(both).substr(3));
	REQUIRE(messages.size() == 2);
	CHECK(messages[0].at("text") == "a");
	CHECK(messages[1].at("text") == "b");
}

TEST_CASE("the decoder rejects an oversized frame") {
	auto const frame = encode_frame(boost::json::value{{"type", "log"}, {"text", std::string(64, 'x')}});
	frame_decoder decoder(16);
	CHECK_THROWS(decoder.feed(frame));
}

TEST_CASE("the engine rejects a run without a parent session") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.subagents_spec());
		co_await rt.mount(h.workflow_spec());
		co_await rt.wait_idle();
		auto engine = rt.root_context().require<workflow_service>(workflow_key).shared();

		start_request request;
		request.script = "return 1;";
		request.meta = boost::json::parse(R"({"name":"x","description":"y"})");
		request.args = boost::json::value(nullptr);
		request.parent = "missing";
		auto result = co_await engine->run(std::move(request));
		CHECK(result.stop_reason == "error");
		REQUIRE(result.error.has_value());
		CHECK(result.error->find("unknown parent session") != std::string::npos);
	});
}

TEST_CASE("the engine reports an actionable error when the node runtime is missing") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.subagents_spec());
		co_await rt.mount(h.workflow_spec_missing_node());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		auto store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		store->create(root, araya::session::session_id{"parent"}, {});
		auto engine = root.require<workflow_service>(workflow_key).shared();

		start_request request;
		request.script = "return 1;";
		request.meta = boost::json::parse(R"({"name":"x","description":"y"})");
		request.args = boost::json::value(nullptr);
		request.parent = "parent";
		auto result = co_await engine->run(std::move(request));

		CHECK(result.stop_reason == "error");
		REQUIRE(result.error.has_value());
		CHECK(result.error->find("could not start the Node runtime") != std::string::npos);
		CHECK(result.error->find("/nonexistent/araya-node-not-here") != std::string::npos);
		CHECK(result.error->find("install Node.js") != std::string::npos);
	});
}

TEST_CASE("a workflow script drives a child agent through the node worker") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		if (!node_available()) {
			SUCCEED("node is not available");
			co_return;
		}
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.subagents_spec());
		co_await rt.mount(h.workflow_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		auto store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		store->create(root, araya::session::session_id{"parent"}, {});
		auto engine = root.require<workflow_service>(workflow_key).shared();

		start_request request;
		request.script = "const a = await agent('hello', {provider: 'mock', model: 'mock-model'});\n"
						 "return { got: a, doubled: 21 * 2 };";
		request.meta = boost::json::parse(R"({"name":"smoke","description":"smoke test"})");
		request.args = boost::json::value(nullptr);
		request.parent = "parent";
		auto result = co_await engine->run(std::move(request));

		REQUIRE(result.stop_reason == "completed");
		REQUIRE(result.value.has_value());
		CHECK(result.value->at("got") == "echo: hello");
		CHECK(result.value->at("doubled").as_int64() == 42);
		CHECK(result.agents_started == 1);
	});
}
