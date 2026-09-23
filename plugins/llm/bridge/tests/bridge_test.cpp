#include <catch2/catch_test_macros.hpp>

#include "araya/llm/bridge.hpp"

#include <boost/json/value.hpp>

#include <string>
#include <vector>

namespace {

using namespace araya::llm_bridge;
using araya::session::message_role;
using araya::session::session_message;

session_message make_message(message_role role, boost::json::value content) {
	session_message result;
	result.role = role;
	result.id = "m1";
	result.content = std::move(content);
	return result;
}

} // namespace

TEST_CASE("to_llm_message converts the surface block vocabulary") {
	auto user = make_message(
		message_role::user,
		boost::json::array{
			{{"type", "text"}, {"text", "hello"}},
			{{"type", "tool_result"},
			 {"tool_call_id", "call-1"},
			 {"content", boost::json::array{{{"type", "text"}, {"text", "ok"}}}},
			 {"is_error", false}},
			{{"type", "unknown"}, {"text", "dropped"}}});
	auto converted = to_llm_message(user);
	CHECK(converted.role == araya::llm::message_role::user);
	REQUIRE(converted.content.size() == 2);
	auto const* text = std::get_if<araya::llm::text_block>(&converted.content[0]);
	REQUIRE(text != nullptr);
	CHECK(text->text == "hello");
	auto const* tool = std::get_if<araya::llm::tool_result_block>(&converted.content[1]);
	REQUIRE(tool != nullptr);
	CHECK(tool->tool_call_id == "call-1");
	CHECK(tool->is_error == false);
	CHECK(tool->content.is_array());
}

TEST_CASE("to_llm_message maps roles and assistant tool calls") {
	auto assistant = make_message(
		message_role::assistant,
		boost::json::array{{{"type", "tool_call"}, {"id", "c1"}, {"name", "echo"}, {"arguments", "{}"}}});
	auto converted = to_llm_message(assistant);
	CHECK(converted.role == araya::llm::message_role::assistant);
	REQUIRE(converted.content.size() == 1);
	auto const* call = std::get_if<araya::llm::tool_call_block>(&converted.content[0]);
	REQUIRE(call != nullptr);
	CHECK(call->id == "c1");
	CHECK(call->name == "echo");
	CHECK(call->arguments == "{}");

	// The session's tool_result role projects onto the llm user role.
	auto tool_result = make_message(message_role::tool_result, boost::json::array{});
	CHECK(to_llm_message(tool_result).role == araya::llm::message_role::user);

	auto system = make_message(message_role::system, boost::json::array{});
	CHECK(to_llm_message(system).role == araya::llm::message_role::system);
}

TEST_CASE("blocks_to_json round-trips every block kind") {
	std::vector<araya::llm::content_block> blocks{
		araya::llm::text_block{"hi"},
		araya::llm::reasoning_block{"thinking"},
		araya::llm::tool_call_block{"c1", "echo", "{}"},
		araya::llm::tool_result_block{"c1", boost::json::value{"done"}, true},
	};
	auto json = blocks_to_json(blocks).as_array();
	REQUIRE(json.size() == 4);
	CHECK(json[0].at("type") == "text");
	CHECK(json[0].at("text") == "hi");
	CHECK(json[1].at("type") == "reasoning");
	CHECK(json[2].at("type") == "tool_call");
	CHECK(json[2].at("id") == "c1");
	CHECK(json[3].at("type") == "tool_result");
	CHECK(json[3].at("tool_call_id") == "c1");
	CHECK(json[3].at("is_error") == true);
}

TEST_CASE("assistant_message_data wraps the message with usage and step siblings") {
	std::vector<araya::llm::content_block> blocks{araya::llm::text_block{"hi"}};
	araya::llm::token_usage usage;
	usage.input_tokens = 3;
	usage.output_tokens = 2;
	usage.total_tokens = 5;

	// Defaults: no turn/step/interrupted siblings.
	auto plain = assistant_message_data("a1", blocks, usage).as_object();
	CHECK(plain.at("message").at("id") == "a1");
	CHECK(plain.at("message").at("content").as_array().size() == 1);
	CHECK(plain.at("usage").at("input_tokens") == 3);
	CHECK(plain.at("usage").at("total_tokens") == 5);
	CHECK(!plain.contains("turn"));
	CHECK(!plain.contains("step"));
	CHECK(!plain.contains("interrupted"));

	auto stepped = assistant_message_data("a2", blocks, usage, std::nullopt, 2, 1, true).as_object();
	CHECK(stepped.at("turn") == 2);
	CHECK(stepped.at("step") == 1);
	CHECK(stepped.at("interrupted") == true);
}

TEST_CASE("the built-in envelopes carry the store's validated shapes") {
	auto user = user_message_data("u1", "hi").as_object();
	CHECK(user.at("role") == "user");
	CHECK(user.at("content").as_array().size() == 1);

	auto system = system_message_data("s1", "tests", "be helpful").as_object();
	auto const& source = system.at("message").at("source");
	CHECK(source.at("kind") == "plugin");
	CHECK(source.at("plugin") == "tests");

	auto tool =
		tool_result_data("t1", "call-9", boost::json::array{{{"type", "text"}, {"text", "ok"}}}, false).as_object();
	CHECK(tool.at("role") == "user");
	auto const& content = tool.at("content").as_array();
	REQUIRE(content.size() == 1);
	CHECK(content[0].at("type") == "tool_result");
	CHECK(content[0].at("tool_call_id") == "call-9");
	CHECK(tool.at("source").at("call_id") == "call-9");
}

TEST_CASE("message_text joins text blocks only") {
	auto message_text_case = make_message(
		message_role::system,
		boost::json::array{
			{{"type", "text"}, {"text", "be "}},
			{{"type", "reasoning"}, {"text", "hidden"}},
			{{"type", "text"}, {"text", "helpful"}}});
	CHECK(message_text(message_text_case) == "be helpful");
}
