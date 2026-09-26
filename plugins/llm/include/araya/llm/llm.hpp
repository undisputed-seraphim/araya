#pragma once

#include "araya/plugin_context.hpp"
#include "araya/service.hpp"
#include "araya/task.hpp"

#include <boost/json/value.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The LLM seam: a strand-confined registry of provider routes plus the
// typed vocabulary of one model call. Feature-replication of the
// deepseek-harness llm package, not a 1-to-1 port.
//
// Error contract: provider and transport failures arrive as a terminal
// finish_chunk carrying an llm_failure - stream() never throws them.
// Only registry errors (no adapter, duplicate route) and malformed
// options throw llm_error.
namespace araya::llm {

// -- roles, blocks, messages ------------------------------------------------

enum class message_role : std::uint8_t {
	system,
	user,
	assistant,
};

enum class content_block_type : std::uint8_t {
	text,
	reasoning,
	tool_call,
	tool_result,
};

// Plain text visible to the end user.
struct text_block {
	std::string text;
};

// Reasoning / thinking content, distinct from visible text.
struct reasoning_block {
	std::string text;
};

// A tool invocation requested by the model. `arguments` is the raw JSON
// string exactly as produced by the model: deltas arrive as fragments,
// so a partial parse must never be attempted.
struct tool_call_block {
	std::string id;
	std::string name;
	std::string arguments;
};

// The result of a tool invocation. `content` mirrors the session
// surface's block array (JSON) - tool results never flow through the v1
// chat surface, so fidelity wins over a typed recursive variant.
struct tool_result_block {
	std::string tool_call_id;
	boost::json::value content;
	bool is_error = false;
};

// An image attached to a message (typically a tool result from `read_image`).
// Local v1 carries the base64 bytes inline so the request path is
// self-contained; the durable attachment store is referenced by id but the
// adapters never resolve it.
struct image_block {
	std::string attachment_id;
	std::string media_type;
	std::string data; // base64-encoded bytes
};

using content_block = std::variant<text_block, reasoning_block, tool_call_block, tool_result_block, image_block>;

struct llm_message {
	message_role role = message_role::user;
	std::vector<content_block> content;
	// Opaque adapter-owned replay data (reasoning signatures and the
	// like): produced on finish, persisted with the assistant message,
	// and handed back verbatim on the next request.
	std::optional<boost::json::value> replay_state;
};

// -- failures ---------------------------------------------------------------

enum class llm_error_code : std::uint8_t {
	no_adapter,
	duplicate_adapter,
	auth,
	rate_limit,
	context_window_exceeded,
	empty_response,
	invalid_request,
	quota,
	server,
	timeout,
	transport,
	aborted,
	stream_closed,
	malformed_response,
};

struct llm_failure {
	llm_failure() = default;

	llm_failure(llm_error_code code, std::string message)
		: code(code)
		, message(std::move(message)) {}

	llm_error_code code = llm_error_code::server;
	std::string message;
	// The provider's own machine code, verbatim from the wire (e.g.
	// "content_filter", "insufficient_quota"); empty when the provider
	// sent none. Diagnostic only - routing stays on `code`.
	std::string provider_code;
	int status = 0; // 0 = none (no valid HTTP status is 0)
	std::optional<std::chrono::milliseconds> provider_retry_after;
	std::string request_id; // empty = none

	// The display code: the provider's string when present, the routing
	// code's name otherwise. Both point at stable storage (this member
	// or a static literal), so the pointer lives as long as the failure.
	char const* code_string() const noexcept;
};

// Thrown for registry and malformed-option errors only; provider and
// transport failures are finish chunks carrying an llm_failure.
class llm_error : public std::runtime_error {
public:
	explicit llm_error(llm_failure failure);

	llm_failure const& failure() const noexcept { return failure_; }

	static char const* code_name(llm_error_code code) noexcept;

private:
	llm_failure failure_;
};

// -- usage, models, tools, options ------------------------------------------

// Counts are disjoint: input_tokens is uncached input only; cached input
// is reported separately. Billed input = input + cacheRead + cacheWrite.
struct token_usage {
	std::uint64_t input_tokens = 0;
	std::uint64_t output_tokens = 0;
	std::optional<std::uint64_t> total_tokens;
	std::optional<std::uint64_t> cache_read_tokens;
	std::optional<std::uint64_t> cache_write_tokens;
	std::optional<std::uint64_t> reasoning_tokens;
};

// One advertised model: identity plus context and call-default metadata.
struct model_info {
	std::string provider;
	std::string model; // the exact id passed to generate_options.model
	std::string name;  // display name; defaults to the id
	std::uint64_t context_window = 0;
	std::uint64_t default_max_tokens = 0;
	std::vector<std::string> reasoning_efforts;
	// Whether the model accepts image input (routed through `read_image`).
	bool supports_image = false;
};

struct provider_info {
	std::string id;
	std::string name;
};

// The provider-owned retry policy captured with a route; adapters return
// nullopt to use the normal defaults. The retry arc consumes this.
struct retry_policy {
	std::uint32_t max_retries = 5;
	std::chrono::milliseconds initial_delay{500};
	std::chrono::milliseconds max_delay{10000};
	double jitter_ratio = 0.1;
};

struct tool_schema {
	std::string name;
	std::string description;
	boost::json::value parameters; // JSON Schema object
};

struct generate_options {
	std::string provider;
	std::string model;
	std::string reasoning_effort; // empty = model default
	std::vector<llm_message> messages;
	std::string system; // empty = no system prompt
	std::vector<tool_schema> tools;
	std::optional<double> temperature;
	std::optional<std::uint64_t> max_tokens;
	std::vector<std::string> stop;
	std::stop_token stop_token;
	std::string session_id; // empty = none
};

// -- the chunk stream -------------------------------------------------------

struct block_start_chunk {
	std::size_t index = 0;
	content_block_type type = content_block_type::text;
};

struct text_delta_chunk {
	std::size_t index = 0;
	std::string text;
};

struct reasoning_delta_chunk {
	std::size_t index = 0;
	std::string text;
};

struct tool_call_delta_chunk {
	std::size_t index = 0;
	std::string id;
	std::string name;			 // empty until the wire carries it (first fragment only)
	std::string arguments_delta; // raw JSON fragment
};

struct block_end_chunk {
	std::size_t index = 0;
	content_block block;
};

struct usage_chunk {
	token_usage usage;
};

struct finish_chunk {
	enum class reason : std::uint8_t {
		stop,
		tool_calls,
		max_tokens,
		error,
		aborted,
	};

	reason why = reason::stop;
	std::optional<llm_failure> failure; // set iff why is error or aborted
	std::optional<boost::json::value> replay_state;
};

// The stream contract: block_start precedes that block's deltas,
// block_end carries the assembled block, usage precedes finish, and
// nothing follows finish.
using stream_chunk = std::variant<
	block_start_chunk,
	text_delta_chunk,
	reasoning_delta_chunk,
	tool_call_delta_chunk,
	block_end_chunk,
	usage_chunk,
	finish_chunk>;

// The stream is pushed through an awaitable callback: adapters co_await
// the sink so consumers can backpressure from inside it.
using chunk_sink = std::function<araya::task<void>(stream_chunk const&)>;

// The terminal chunk for a failure: aborted when the code says so,
// error otherwise (the adapters' shared failure exit).
inline finish_chunk finish_from(llm_failure failure) {
	finish_chunk finish;
	finish.why = failure.code == llm_error_code::aborted ? finish_chunk::reason::aborted : finish_chunk::reason::error;
	finish.failure = std::move(failure);
	return finish;
}

// The durable {code, message} shape sessions log for a failure (the
// turn/end and assistant/attempt events).
inline boost::json::value failure_to_json(llm_failure const& failure) {
	return boost::json::value{
		{"code", llm_error::code_name(failure.code)},
		{"message", failure.message},
	};
}

// -- the adapter interface --------------------------------------------------

class llm_adapter {
public:
	virtual ~llm_adapter() = default;

	// Streams one call as raw chunks. The only required method: honor
	// options.stop_token, and never throw provider or transport failures
	// (they are finish chunks).
	virtual araya::task<void> stream(generate_options const& options, chunk_sink const& sink) = 0;

	// Advisory catalog in adapter-preferred order; absence must not turn
	// into request rejection.
	virtual std::vector<model_info> list_models(std::string_view);

	virtual model_info resolve_model(std::string_view provider, std::string_view model);

	virtual provider_info describe_provider(std::string_view provider);

	virtual std::optional<retry_policy> provider_retry_policy(std::string_view);
};

// -- the service ------------------------------------------------------------

// The strand-confined registry: routes register inside a plugin's apply
// (the control strand), and stream() dispatches from command handlers on
// the same strand.
class llm_service {
public:
	// All-or-nothing: if any provider already has a route, nothing is
	// registered and llm_error{duplicate_adapter} is thrown. The returned
	// registration erases the routes early; the registering fiber's
	// teardown does it otherwise (the effect is tracked through
	// plugin_context::effect).
	araya::registration register_adapter(
		std::vector<std::string> providers,
		std::shared_ptr<llm_adapter> adapter,
		araya::plugin_context& caller);

	// Routes provider -> adapter and delegates. Throws
	// llm_error{no_adapter} for unknown providers and
	// std::invalid_argument for empty provider/model; provider failures
	// never throw here.
	araya::task<void> stream(generate_options const& options, chunk_sink const& sink);

	std::vector<std::string> providers() const;

	// The first provider route's id (map order, matching providers().front()),
	// or nullopt when no route is registered. Avoids materializing the list
	// for the common single-provider call.
	std::optional<std::string_view> first_provider() const;

	std::optional<model_info> resolve_model(std::string_view provider, std::string_view model) const;

private:
	struct route {
		std::shared_ptr<llm_adapter> adapter;
	};

	std::map<std::string, route, std::less<>> routes_;
};

inline constexpr araya::service_key<llm_service> llm_key{"llm", 1};

// The plugin descriptor: apply() constructs the service and provides it
// under llm_key.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::llm
