#pragma once

#include "araya/llm/llm.hpp"

#include <boost/json/value.hpp>

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The chat-completions wire translation: pure JSON <-> chunks, no I/O.
// Faithful to the deepseek-harness chat-completions protocol (request
// build, SSE delta mapping, usage split, status -> failure).
namespace araya::llm_openai {

using araya::llm::chunk_sink;
using araya::llm::finish_chunk;
using araya::llm::generate_options;
using araya::llm::llm_error;
using araya::llm::llm_error_code;
using araya::llm::llm_failure;
using araya::llm::stream_chunk;
using araya::llm::token_usage;

// Builds the POST /chat/completions body. Reasoning blocks are never
// replayed (DeepSeek forbids sending reasoning_content back); tool
// results serialize as role "tool" messages.
boost::json::value build_request(generate_options const& options);

// Maps a non-2xx status plus its error body to a stable failure code.
// Quota and context-window detection look at the provider's error
// detail text. Retry-After (seconds or HTTP-date) and the request id
// ride along when present.
llm_failure failure_for(
	unsigned status,
	std::string_view body,
	std::optional<std::chrono::milliseconds> retry_after,
	std::optional<std::string> request_id);

// Parses a Retry-After header value: integer seconds or an HTTP-date.
std::optional<std::chrono::milliseconds> parse_retry_after(std::string_view value);

// The stateful SSE payload translator: one open block per content,
// reasoning, or tool-call index. Finish reason and the latest usage are
// deferred until finish() (the [DONE] sentinel), so nothing follows
// finish. Malformed payloads throw llm_error{malformed_response}.
class chunk_translator {
public:
	// Consumes one parsed SSE data payload and returns the chunks it
	// produced. Throws llm_error{malformed_response} on bad shapes.
	std::vector<stream_chunk> feed(boost::json::value const& wire);

	// The [DONE] sentinel: block-ends in open order, usage, then finish.
	// A stop finish with no opened blocks maps to an empty_response
	// error. After finish(), further feed() throws.
	std::vector<stream_chunk> finish();

	bool done() const noexcept { return done_; }

private:
	struct open_block {
		std::size_t index = 0;
		araya::llm::content_block_type kind = araya::llm::content_block_type::text;
		std::string text;
		std::string call_id;
		std::optional<std::string> name;
	};

	open_block& open(araya::llm::content_block_type kind);

	std::vector<open_block> order_;
	open_block* text_ = nullptr;
	open_block* reasoning_ = nullptr;
	std::map<std::size_t, std::size_t> tool_position_; // wire index -> position in order_
	std::optional<finish_chunk::reason> pending_finish_;
	std::optional<llm_failure> pending_failure_;
	std::optional<token_usage> pending_usage_;
	std::size_t next_index_ = 0;
	bool done_ = false;
};

} // namespace araya::llm_openai
