#pragma once

#include "araya/llm/llm.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Shared builders for the llm seam's chunk vocabulary.
namespace araya_test::llm {

using araya::llm::stream_chunk;

inline stream_chunk text_delta(std::size_t index, std::string text) {
	araya::llm::text_delta_chunk chunk;
	chunk.index = index;
	chunk.text = std::move(text);
	return chunk;
}

inline stream_chunk usage(std::uint64_t in, std::uint64_t out) {
	araya::llm::usage_chunk chunk;
	chunk.usage.input_tokens = in;
	chunk.usage.output_tokens = out;
	return chunk;
}

inline stream_chunk finish(araya::llm::finish_chunk::reason why) {
	araya::llm::finish_chunk chunk;
	chunk.why = why;
	return chunk;
}

// A complete text answer: block start, one delta, block end, usage, and
// a stop finish.
inline std::vector<stream_chunk> text_stream(std::string text) {
	araya::llm::token_usage tokens;
	tokens.input_tokens = 3;
	tokens.output_tokens = 2;
	return {
		stream_chunk{araya::llm::block_start_chunk{0, araya::llm::content_block_type::text}},
		stream_chunk{araya::llm::text_delta_chunk{0, text}},
		stream_chunk{araya::llm::block_end_chunk{0, araya::llm::content_block{araya::llm::text_block{text}}}},
		stream_chunk{araya::llm::usage_chunk{tokens}},
		stream_chunk{araya::llm::finish_chunk{araya::llm::finish_chunk::reason::stop, std::nullopt, std::nullopt}},
	};
}

// A complete tool call: block start, identity + arguments delta, block
// end with the assembled call, usage, and a tool_calls finish.
inline std::vector<stream_chunk> tool_stream(std::string name, std::string arguments) {
	araya::llm::tool_call_block call{"call-1", name, arguments};
	araya::llm::token_usage tokens;
	tokens.input_tokens = 4;
	tokens.output_tokens = 1;
	return {
		stream_chunk{araya::llm::block_start_chunk{0, araya::llm::content_block_type::tool_call}},
		stream_chunk{araya::llm::tool_call_delta_chunk{0, "call-1", name, arguments}},
		stream_chunk{araya::llm::block_end_chunk{0, araya::llm::content_block{call}}},
		stream_chunk{araya::llm::usage_chunk{tokens}},
		stream_chunk{
			araya::llm::finish_chunk{araya::llm::finish_chunk::reason::tool_calls, std::nullopt, std::nullopt}},
	};
}

// A terminal error finish carrying the given failure message.
inline std::vector<stream_chunk> error_stream(std::string message) {
	araya::llm::llm_failure failure{araya::llm::llm_error_code::server, std::move(message)};
	return {
		stream_chunk{
			araya::llm::finish_chunk{araya::llm::finish_chunk::reason::error, std::move(failure), std::nullopt}},
	};
}

// One SSE document: data lines joined by blank lines, optionally closed
// by the [DONE] sentinel.
inline std::string sse(std::vector<std::string> const& payloads, bool with_done = true) {
	std::string result;
	for (auto const& payload : payloads)
		result += "data: " + payload + "\n\n";
	if (with_done)
		result += "data: [DONE]\n\n";
	return result;
}

} // namespace araya_test::llm
