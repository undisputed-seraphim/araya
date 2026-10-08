#include <catch2/catch_test_macros.hpp>

#include "araya/llm-compat/compat.hpp"
#include "araya/llm/llm.hpp"
#include "messages.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <optional>
#include <string>
#include <vector>

namespace {

using namespace araya::llm;
using namespace araya::llm_compat;

boost::json::value event(std::string_view json) { return boost::json::parse(json); }

std::vector<stream_chunk> feed(messages_translator& translator, std::string_view json) {
	return translator.feed(event(json));
}

std::optional<finish_chunk> finish_of(std::vector<stream_chunk> const& chunks) {
	if (chunks.empty())
		return std::nullopt;
	auto const* finish = std::get_if<finish_chunk>(&chunks.back());
	return finish ? std::optional<finish_chunk>(*finish) : std::nullopt;
}

compat_model model_with(std::string id = "m") {
	compat_model model;
	model.id = std::move(id);
	model.default_max_tokens = 2048;
	return model;
}

} // namespace

TEST_CASE("messages request serializes text, system, and tools") {
	compat_config config;
	config.fallback_max_tokens = 1000;
	generate_options options;
	options.model = "m";
	options.system = "be brief";
	options.messages = {{message_role::user, {text_block{"hello"}}, std::nullopt}};
	options.tools = {tool_schema{"echo", "Echo text.", boost::json::object{{"type", "object"}}}};

	auto const wire = build_messages_request(options, config, model_with()).as_object();
	CHECK(wire.at("model") == "m");
	CHECK(wire.at("stream") == true);
	CHECK(wire.at("max_tokens") == 2048); // model default wins over the config fallback
	CHECK(wire.at("system") == "be brief");
	CHECK(wire.at("messages").as_array().size() == 1);
	CHECK(wire.at("messages").as_array()[0].as_object().at("role") == "user");
	CHECK(wire.at("thinking").as_object().at("type") == "enabled");
	CHECK(wire.at("output_config").as_object().at("effort") == "high");
	CHECK(wire.at("tools").as_array()[0].as_object().at("name") == "echo");
}

TEST_CASE("messages reasoning effort off and Anthropic budget mode") {
	generate_options options;
	options.model = "m";
	options.messages = {{message_role::user, {text_block{"hi"}}, std::nullopt}};

	{
		options.reasoning_effort = "off";
		compat_config config;
		auto const wire = build_messages_request(options, config, model_with()).as_object();
		CHECK(wire.at("thinking").as_object().at("type") == "disabled");
		CHECK_FALSE(wire.contains("output_config"));
	}
	{
		options.reasoning_effort = "high";
		compat_config config;
		config.thinking_budget_tokens = 4096;
		auto const wire = build_messages_request(options, config, model_with()).as_object();
		CHECK(wire.at("thinking").as_object().at("type") == "enabled");
		CHECK(wire.at("thinking").as_object().at("budget_tokens") == 4096);
		CHECK_FALSE(wire.contains("output_config"));
	}
}

TEST_CASE("messages keeps in-history system updates after the user turn") {
	compat_config config;
	generate_options options;
	options.model = "m";
	options.system = "top";
	auto model = model_with();
	model.system_prompt_update = std::string{"in-history"};
	options.messages = {
		{message_role::user, {text_block{"first"}}, std::nullopt},
		{message_role::system, {text_block{"update"}}, std::nullopt},
	};

	auto const wire = build_messages_request(options, config, model).as_object();
	CHECK(wire.at("system") == "top");
	auto const& messages = wire.at("messages").as_array();
	REQUIRE(messages.size() == 2);
	CHECK(messages[0].as_object().at("role") == "user");
	CHECK(messages[1].as_object().at("role") == "system");
	CHECK(messages[1].as_object().at("content").as_array()[0].as_object().at("text") == "update");
}

TEST_CASE("messages translator assembles text, usage, and finish") {
	messages_translator translator("m");
	feed(translator, R"({"type":"message_start","message":{"usage":{"input_tokens":5,"output_tokens":0}}})");
	auto start =
		feed(translator, R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})");
	REQUIRE(start.size() == 1);
	CHECK(std::holds_alternative<block_start_chunk>(start[0]));

	auto delta =
		feed(translator, R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hi"}})");
	REQUIRE(delta.size() == 1);
	CHECK(std::get<text_delta_chunk>(delta[0]).text == "Hi");

	auto stop = feed(translator, R"({"type":"content_block_stop","index":0})");
	REQUIRE(stop.size() == 1);
	CHECK(std::holds_alternative<text_block>(std::get<block_end_chunk>(stop[0]).block));

	auto message_delta =
		feed(translator, R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":2}})");
	CHECK(message_delta.empty());

	auto ended = feed(translator, R"({"type":"message_stop"})");
	REQUIRE(ended.size() == 2);
	CHECK(std::get<usage_chunk>(ended[0]).usage.input_tokens == 5);
	CHECK(std::get<usage_chunk>(ended[0]).usage.output_tokens == 2);
	auto const finish = finish_of(ended);
	REQUIRE(finish.has_value());
	CHECK(finish->why == finish_chunk::reason::stop);
	REQUIRE(finish->replay_state.has_value());
	CHECK(finish->replay_state->as_object().at("format") == "messages");
	CHECK(translator.done());
}

TEST_CASE("messages translator assembles a streamed tool use") {
	messages_translator translator("m");
	feed(translator, R"({"type":"message_start","message":{"usage":{"input_tokens":1}}})");
	feed(
		translator,
		R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"t1","name":"echo","input":{}}})");
	feed(
		translator,
		R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"a\":"}})");
	feed(
		translator,
		R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"1}"}})");
	auto stop = feed(translator, R"({"type":"content_block_stop","index":0})");
	REQUIRE(stop.size() == 1);
	auto const* block = std::get_if<tool_call_block>(&std::get<block_end_chunk>(stop[0]).block);
	REQUIRE(block != nullptr);
	CHECK(block->id == "t1");
	CHECK(block->name == "echo");
	CHECK(block->arguments == R"({"a":1})");

	feed(translator, R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})");
	auto ended = feed(translator, R"({"type":"message_stop"})");
	auto const finish = finish_of(ended);
	REQUIRE(finish.has_value());
	CHECK(finish->why == finish_chunk::reason::tool_calls);
}

TEST_CASE("messages translator keeps a thinking signature for replay") {
	messages_translator translator("m");
	feed(translator, R"({"type":"message_start","message":{}})");
	feed(translator, R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})");
	feed(translator, R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"hmm"}})");
	feed(
		translator, R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig"}})");
	feed(translator, R"({"type":"content_block_stop","index":0})");
	feed(translator, R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})");
	auto ended = feed(translator, R"({"type":"message_stop"})");
	auto const finish = finish_of(ended);
	REQUIRE(finish.has_value());
	REQUIRE(finish->replay_state.has_value());
	CHECK(finish->replay_state->as_object().at("signatures").as_array()[0] == "sig");
}

TEST_CASE("messages translator rejects a delta without an open block") {
	messages_translator translator("m");
	feed(translator, R"({"type":"message_start","message":{}})");
	CHECK_THROWS(
		translator.feed(event(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"x"}})")));
}
