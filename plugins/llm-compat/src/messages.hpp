#pragma once

#include "araya/llm-compat/compat.hpp"
#include "araya/llm/llm.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/json/value.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The Anthropic-compatible Messages wire path: request serialization, SSE
// event translation, and error classification. A trimmed port of the
// deepseek-harness `llm-deepseek/protocols/messages` (text, thinking, tool
// use, tool results, inline images; no Files API).
namespace araya::llm_compat {

// Builds the POST /v1/messages body. `model` supplies max-token/reasoning
// defaults; the request's explicit values win.
boost::json::value build_messages_request(
	araya::llm::generate_options const& options,
	compat_config const& config,
	compat_model const& model);

// Maps a non-2xx Messages response to a stable failure: the Anthropic error
// `type` is consulted first, then the HTTP status and message text.
araya::llm::llm_failure messages_failure_for(
	unsigned status,
	std::string_view body,
	std::optional<std::chrono::milliseconds> retry_after,
	std::string request_id);

// The stateful Messages SSE translator. Each event is one decoded `data:`
// JSON object; `feed` returns the chunks it produced. `message_stop` yields
// usage and the terminal finish; a malformed event throws
// llm_error{malformed_response}. After finish, further feed() throws.
class messages_translator {
public:
	explicit messages_translator(std::string model);

	std::vector<araya::llm::stream_chunk> feed(boost::json::value const& event);

	bool done() const noexcept { return done_; }

private:
	struct open_block {
		std::size_t index = 0;
		araya::llm::content_block_type kind = araya::llm::content_block_type::text;
		std::string text;
		std::string call_id;
		std::string name;
		std::string json;
		std::string signature;
		bool closed = false;
	};

	open_block& start_block(boost::json::value const& content_block, std::vector<araya::llm::stream_chunk>& chunks);
	std::vector<araya::llm::stream_chunk> delta(open_block& block, boost::json::value const& delta);
	void stop_block(open_block& block, std::vector<araya::llm::stream_chunk>& chunks);
	boost::json::value replay_state() const;

	std::string model_;
	std::deque<open_block> order_;
	std::map<std::size_t, std::size_t> wire_index_; // wire index -> position in order_
	araya::llm::token_usage usage_;
	std::optional<araya::llm::finish_chunk::reason> pending_finish_;
	std::optional<araya::llm::llm_failure> pending_failure_;
	bool started_ = false;
	bool done_ = false;
};

// Runs one Messages call end to end: POST {base}/v1/messages with the
// Anthropic headers, parse the SSE stream, and drive the translator into
// `sink`. Provider/transport failures arrive as the terminal finish chunk.
araya::task<void> run_messages_call(
	boost::asio::any_io_executor executor,
	compat_config const& config,
	compat_model const& model,
	araya::llm::generate_options const& options,
	araya::llm::chunk_sink const& sink);

} // namespace araya::llm_compat
