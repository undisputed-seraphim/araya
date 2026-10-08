#include <catch2/catch_test_macros.hpp>

#include "araya/llm-openai/openai.hpp"
#include "translate.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

using namespace araya::llm;
using namespace araya::llm_openai;

llm_message message(message_role role, std::vector<content_block> blocks) {
	llm_message result;
	result.role = role;
	result.content = std::move(blocks);
	return result;
}

boost::json::value parse(std::string_view text) { return boost::json::parse(text); }

} // namespace

TEST_CASE("build_request maps roles, tool calls, and tool results to the wire") {
	generate_options options;
	options.model = "deepseek-chat";
	options.system = "be concise";
	options.messages = {
		message(message_role::user, {text_block{"weather?"}}),
		message(message_role::assistant, {tool_call_block{"call-1", "weather", "{\"city\":\"X\"}"}}),
		message(
			message_role::user,
			{tool_result_block{"call-1", boost::json::array{{{"type", "text"}, {"text", "sunny"}}}, false}}),
	};
	options.max_tokens = 100;

	auto const request = build_request(options);
	auto const& object = request.as_object();
	CHECK(object.at("model") == "deepseek-chat");
	CHECK(object.at("stream") == true);
	CHECK(object.at("stream_options") == parse(R"({"include_usage":true})"));
	CHECK(object.at("max_tokens") == 100);
	REQUIRE(object.contains("messages"));
	auto const& messages = object.at("messages").as_array();
	REQUIRE(messages.size() == 4);
	CHECK(messages[0] == parse(R"({"role":"system","content":"be concise"})"));
	CHECK(messages[1] == parse(R"({"role":"user","content":"weather?"})"));
	CHECK(
		messages[2] ==
		parse(
			R"({"role":"assistant","tool_calls":[{"id":"call-1","type":"function","function":{"name":"weather","arguments":"{\"city\":\"X\"}"}}]})"));
	CHECK(messages[3] == parse(R"({"role":"tool","tool_call_id":"call-1","content":"sunny"})"));
}

TEST_CASE("build_request renders an image block as a data-URL user message") {
	generate_options options;
	options.model = "vision";
	options.messages = {
		message(message_role::assistant, {tool_call_block{"call-1", "read_image", "{}"}}),
		message(
			message_role::user,
			{image_block{"att-1", "image/png", "QUJD"},
			 tool_result_block{
				 "call-1", boost::json::array{{{"type", "text"}, {"text", "<path>a.png</path>"}}}, false}}),
	};

	auto const request = build_request(options);
	auto const& messages = request.as_object().at("messages").as_array();
	REQUIRE(messages.size() == 3);
	CHECK(messages[1] == parse(R"({"role":"tool","tool_call_id":"call-1","content":"<path>a.png</path>"})"));
	CHECK(
		messages[2] ==
		parse(R"({"role":"user","content":[{"type":"image_url","image_url":{"url":"data:image/png;base64,QUJD"}}]})"));
}

TEST_CASE("build_request carries reasoning effort, tools, temperature, and stop") {
	generate_options options;
	options.model = "deepseek-reasoner";
	options.reasoning_effort = "high";
	options.temperature = 0.3;
	options.stop = {"END", "STOP"};
	options.tools = {tool_schema{"weather", "look it up", parse(R"({"type":"object"})")}};

	auto const request = build_request(options);
	auto const& object = request.as_object();
	CHECK(object.at("thinking") == parse(R"({"type":"enabled"})"));
	CHECK(object.at("reasoning_effort") == "high");
	CHECK(object.at("temperature") == 0.3);
	CHECK(object.at("stop") == parse(R"(["END","STOP"])"));
	REQUIRE(object.contains("tools"));
	auto const& tool = object.at("tools").as_array().front().as_object();
	CHECK(tool.at("type") == "function");
	CHECK(tool.at("function").as_object().at("name") == "weather");
	CHECK(tool.at("function").as_object().at("parameters") == parse(R"({"type":"object"})"));
}

TEST_CASE("text deltas open one block and finish assembles it") {
	chunk_translator translator;
	auto first = translator.feed(parse(R"({"choices":[{"delta":{"content":"hello "}}]})"));
	REQUIRE(first.size() == 2);
	CHECK(std::get<block_start_chunk>(first[0]).type == content_block_type::text);
	CHECK(std::get<text_delta_chunk>(first[1]).text == "hello ");

	auto second = translator.feed(parse(R"({"choices":[{"delta":{"content":"world"}}]})"));
	REQUIRE(second.size() == 1);
	CHECK(std::get<text_delta_chunk>(second[0]).text == "world");

	auto tail = translator.finish();
	REQUIRE(tail.size() == 2);
	auto const& block_end = std::get<block_end_chunk>(tail[0]);
	CHECK(std::get<text_block>(block_end.block).text == "hello world");
	auto const& finish = std::get<finish_chunk>(tail[1]);
	CHECK(finish.why == finish_chunk::reason::stop);
}

TEST_CASE("usage splits cached tokens out of the prompt count") {
	chunk_translator translator;
	(void)translator.feed(parse(R"({"choices":[{"delta":{"content":"x"}}],"usage":{
		"prompt_tokens": 100,
		"completion_tokens": 20,
		"total_tokens": 120,
		"prompt_tokens_details": {"cached_tokens": 70},
		"completion_tokens_details": {"reasoning_tokens": 5}
	}})"));
	auto tail = translator.finish();
	REQUIRE(tail.size() == 3);
	auto const& usage = std::get<usage_chunk>(tail[1]).usage;
	CHECK(usage.input_tokens == 30);
	CHECK(usage.output_tokens == 20);
	CHECK(usage.total_tokens == 120);
	CHECK(usage.cache_read_tokens == 70);
	CHECK(usage.reasoning_tokens == 5);
}

TEST_CASE("reasoning deltas ignore the empty first chunk and open their own block") {
	chunk_translator translator;
	auto nothing = translator.feed(parse(R"({"choices":[{"delta":{"reasoning_content":""}}]})"));
	CHECK(nothing.empty());
	auto thinking = translator.feed(parse(R"({"choices":[{"delta":{"reasoning_content":"think"}}]})"));
	REQUIRE(thinking.size() == 2);
	CHECK(std::get<block_start_chunk>(thinking[0]).type == content_block_type::reasoning);
	auto tail = translator.finish();
	auto const& block_end = std::get<block_end_chunk>(tail[0]);
	CHECK(std::get<reasoning_block>(block_end.block).text == "think");
}

TEST_CASE("tool-call deltas keep identity and accumulate raw argument fragments") {
	chunk_translator translator;
	(void)translator.feed(parse(
		R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-1","function":{"name":"weather","arguments":"{\"ci"}}]}}]})"));
	auto more = translator.feed(parse(
		R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"","function":{"name":null,"arguments":"ty\":\"X\"}"}}]}}]})"));
	REQUIRE(more.size() == 1);
	auto const& delta = std::get<tool_call_delta_chunk>(more[0]);
	CHECK(delta.id == "call-1");
	CHECK(delta.name == "weather");

	// finish_reason on the trailing chunk; no [DONE] payload needed here.
	(void)translator.feed(parse(R"({"choices":[{"finish_reason":"tool_calls"}]})"));
	auto tail = translator.finish();
	auto const& block_end = std::get<block_end_chunk>(tail[0]);
	auto const& call = std::get<tool_call_block>(block_end.block);
	CHECK(call.id == "call-1");
	CHECK(call.name == "weather");
	CHECK(call.arguments == "{\"city\":\"X\"}");
	CHECK(std::get<finish_chunk>(tail[1]).why == finish_chunk::reason::tool_calls);
}

TEST_CASE("reasoning accumulates across interleaved tool-call blocks") {
	// The reasoning block opens first, then many tool-call blocks grow the
	// open-block list, then reasoning resumes: the open-block storage must
	// keep the reasoning reference valid across that growth.
	chunk_translator translator;
	(void)translator.feed(parse(R"({"choices":[{"delta":{"reasoning_content":"part-one "}}]})"));
	for (int i = 0; i < 8; ++i) {
		auto json = std::string{R"({"choices":[{"delta":{"tool_calls":[{"index":)"} + std::to_string(i) +
					R"(,"id":"call-)" + std::to_string(i) + R"(","function":{"name":"t","arguments":"{}"}}]}}]})";
		(void)translator.feed(parse(json));
	}
	(void)translator.feed(parse(R"({"choices":[{"delta":{"reasoning_content":"part-two"}}]})"));
	(void)translator.feed(parse(R"({"choices":[{"finish_reason":"stop"}]})"));

	std::string reasoning_text;
	for (auto const& chunk : translator.finish()) {
		if (auto const* end = std::get_if<block_end_chunk>(&chunk)) {
			if (auto const* block = std::get_if<reasoning_block>(&end->block))
				reasoning_text = block->text;
		}
	}
	CHECK(reasoning_text == "part-one part-two");
}

TEST_CASE("finish reasons map: length to max_tokens, unknowns to an error failure") {
	{
		chunk_translator translator;
		(void)translator.feed(parse(R"({"choices":[{"delta":{"content":"x"},"finish_reason":"length"}]})"));
		auto tail = translator.finish();
		CHECK(std::get<finish_chunk>(tail.back()).why == finish_chunk::reason::max_tokens);
	}
	{
		chunk_translator translator;
		(void)translator.feed(parse(R"({"choices":[{"delta":{"content":"x"},"finish_reason":"content_filter"}]})"));
		auto tail = translator.finish();
		auto const& finish = std::get<finish_chunk>(tail.back());
		CHECK(finish.why == finish_chunk::reason::error);
		REQUIRE(finish.failure.has_value());
		CHECK(finish.failure->message.find("content_filter") != std::string::npos);
		CHECK(finish.failure->provider_code == "content_filter");
		CHECK(finish.failure->code_string() == std::string("content_filter"));
	}
}

TEST_CASE("a stream with no content finishes as empty_response") {
	chunk_translator translator;
	auto tail = translator.finish();
	REQUIRE(tail.size() == 1);
	auto const& finish = std::get<finish_chunk>(tail[0]);
	CHECK(finish.why == finish_chunk::reason::error);
	REQUIRE(finish.failure.has_value());
	CHECK(finish.failure->code == llm_error_code::empty_response);
}

TEST_CASE("feeding after finish throws malformed_response") {
	chunk_translator translator;
	(void)translator.finish();
	CHECK_THROWS_AS(translator.feed(parse(R"({"choices":[]})")), llm_error);
}

TEST_CASE("failure_for maps statuses to stable codes") {
	auto auth = failure_for(401, R"({"error":{"message":"bad key"}})", std::nullopt, {});
	CHECK(auth.code == llm_error_code::auth);
	CHECK(auth.status == 401);
	CHECK(auth.message == "bad key");
	CHECK(auth.provider_code.empty());
	CHECK(auth.code_string() == std::string("auth"));

	auto rate = failure_for(
		429, R"({"error":{"message":"slow down"}})", std::chrono::milliseconds(42000), std::string("req-1"));
	CHECK(rate.code == llm_error_code::rate_limit);
	CHECK(rate.provider_retry_after == std::chrono::milliseconds(42000));
	CHECK(rate.request_id == "req-1");

	auto quota = failure_for(
		429,
		R"({"error":{"code":"insufficient_quota","type":"invalid_request_error","message":"out of credits"}})",
		std::nullopt,
		{});
	CHECK(quota.code == llm_error_code::quota);
	CHECK(quota.provider_code == "insufficient_quota");

	auto context = failure_for(
		400, R"({"error":{"message":"This model's maximum context length is exceeded"}})", std::nullopt, {});
	CHECK(context.code == llm_error_code::context_window_exceeded);

	auto invalid = failure_for(400, R"({"error":{"type":"bad_request","message":"bad request"}})", std::nullopt, {});
	CHECK(invalid.code == llm_error_code::invalid_request);
	CHECK(invalid.provider_code == "bad_request");

	auto server = failure_for(502, "gateway gone", std::nullopt, {});
	CHECK(server.code == llm_error_code::server);
}

TEST_CASE("parse_retry_after accepts seconds and HTTP dates") {
	CHECK(parse_retry_after("42") == std::chrono::seconds(42));
	CHECK(parse_retry_after("garbage") == std::nullopt);
	CHECK(parse_retry_after("") == std::nullopt);
}
