#include "messages.hpp"

#include "logging.hpp"

#include "araya/llm/http.hpp"
#include "araya/llm/sse.hpp"
#include "araya/util/json.hpp"
#include "translate.hpp"

#include <boost/asio/this_coro.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace araya::llm_compat {
namespace {

using araya::llm::chunk_sink;
using araya::llm::content_block_type;
using araya::llm::finish_chunk;
using araya::llm::finish_from;
using araya::llm::generate_options;
using araya::llm::llm_error;
using araya::llm::llm_error_code;
using araya::llm::llm_failure;
using araya::llm::llm_message;
using araya::llm::stream_chunk;
using araya::llm::token_usage;

std::string join_text(llm_message const& message) {
	std::string text;
	for (auto const& block : message.content) {
		if (auto const* t = std::get_if<araya::llm::text_block>(&block))
			text += t->text;
	}
	return text;
}

llm_failure malformed(std::string detail) {
	return llm_failure{llm_error_code::malformed_response, "Messages stream: " + std::move(detail)};
}

// The signature for assistant content block `index` from the opaque replay
// state, or nullopt when absent.
std::optional<std::string> replay_signature(llm_message const& message, std::size_t index) {
	if (!message.replay_state || !message.replay_state->is_object())
		return std::nullopt;
	auto const& object = message.replay_state->as_object();
	auto const* format = object.if_contains("format");
	if (!format || !format->is_string() || format->as_string() != "messages")
		return std::nullopt;
	auto const* signatures = object.if_contains("signatures");
	if (!signatures || !signatures->is_array())
		return std::nullopt;
	auto const& array = signatures->as_array();
	if (index >= array.size() || !array[index].is_string())
		return std::nullopt;
	return std::string(array[index].as_string());
}

boost::json::object tool_result_content(boost::json::value const& content) {
	std::string text;
	if (content.is_array()) {
		for (auto const& inner : content.as_array()) {
			auto const* node = inner.if_object();
			if (!node)
				continue;
			auto const* type = node->if_contains("type");
			auto const* value = node->if_contains("text");
			if (type && type->is_string() && type->as_string() == "text" && value && value->is_string())
				text += value->as_string();
		}
	} else if (content.is_string()) {
		text = std::string(content.as_string());
	}
	return boost::json::object{{"type", "text"}, {"text", std::move(text)}};
}

boost::json::array assistant_blocks(llm_message const& message) {
	boost::json::array blocks;
	std::size_t index = 0;
	for (auto const& block : message.content) {
		if (auto const* text = std::get_if<araya::llm::text_block>(&block)) {
			blocks.emplace_back(boost::json::object{{"type", "text"}, {"text", text->text}});
		} else if (auto const* reasoning = std::get_if<araya::llm::reasoning_block>(&block)) {
			boost::json::object entry{{"type", "thinking"}, {"thinking", reasoning->text}};
			if (auto signature = replay_signature(message, index))
				entry["signature"] = *signature;
			blocks.emplace_back(std::move(entry));
		} else if (auto const* call = std::get_if<araya::llm::tool_call_block>(&block)) {
			boost::json::value input;
			try {
				input = boost::json::parse(call->arguments);
			} catch (...) {
				throw llm_error(llm_failure{llm_error_code::invalid_request, "Messages tool input is invalid JSON"});
			}
			if (!input.is_object())
				throw llm_error(llm_failure{llm_error_code::invalid_request, "Messages tool input must be an object"});
			blocks.emplace_back(boost::json::object{
				{"type", "tool_use"}, {"id", call->id}, {"name", call->name}, {"input", std::move(input)}});
		}
		++index;
	}
	return blocks;
}

boost::json::array user_blocks(llm_message const& message) {
	boost::json::array blocks;
	for (auto const& block : message.content) {
		if (auto const* text = std::get_if<araya::llm::text_block>(&block)) {
			if (!text->text.empty())
				blocks.emplace_back(boost::json::object{{"type", "text"}, {"text", text->text}});
		} else if (auto const* image = std::get_if<araya::llm::image_block>(&block)) {
			blocks.emplace_back(boost::json::object{
				{"type", "image"},
				{"source",
				 boost::json::object{{"type", "base64"}, {"media_type", image->media_type}, {"data", image->data}}}});
		} else if (auto const* result = std::get_if<araya::llm::tool_result_block>(&block)) {
			boost::json::object entry{
				{"type", "tool_result"},
				{"tool_use_id", result->tool_call_id},
				{"content", boost::json::array{tool_result_content(result->content)}}};
			if (result->is_error)
				entry["is_error"] = true;
			blocks.emplace_back(std::move(entry));
		}
	}
	return blocks;
}

std::string role_name(araya::llm::message_role role) {
	switch (role) {
	case araya::llm::message_role::assistant:
		return "assistant";
	case araya::llm::message_role::user:
		return "user";
	case araya::llm::message_role::system:
		return "system";
	}
	return "user";
}

bool valid_effort(std::string_view effort) {
	return effort == "off" || effort == "low" || effort == "high" || effort == "max";
}

} // namespace

boost::json::value
build_messages_request(generate_options const& options, compat_config const& config, compat_model const& model) {
	bool const in_history = model.system_prompt_update && *model.system_prompt_update == "in-history";

	boost::json::array messages;
	std::string history_system;
	std::vector<boost::json::value> system_updates;
	auto flush_system_updates = [&] {
		if (system_updates.empty())
			return;
		bool const after_user = !messages.empty() && messages.back().is_object() &&
								araya::util::json::get_string(messages.back().as_object(), "role") == "user";
		if (!after_user)
			throw llm_error(
				llm_failure{llm_error_code::invalid_request, "Messages system update without a preceding user turn"});
		for (auto& update : system_updates)
			messages.push_back(std::move(update));
		system_updates.clear();
	};

	for (auto const& message : options.messages) {
		if (message.role == araya::llm::message_role::system) {
			std::string const text = join_text(message);
			if (in_history && !messages.empty()) {
				if (text.empty())
					throw llm_error(
						llm_failure{llm_error_code::invalid_request, "Messages in-history system update is empty"});
				system_updates.push_back(boost::json::object{
					{"role", "system"},
					{"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", text}}}}});
			} else {
				history_system = text;
			}
			continue;
		}
		if (message.role == araya::llm::message_role::assistant)
			flush_system_updates();
		std::string const role = role_name(message.role);
		boost::json::array content =
			message.role == araya::llm::message_role::assistant ? assistant_blocks(message) : user_blocks(message);
		if (!messages.empty() && messages.back().is_object() &&
			araya::util::json::get_string(messages.back().as_object(), "role") == role) {
			auto& previous = messages.back().as_object()["content"].as_array();
			for (auto& block : content)
				previous.push_back(std::move(block));
		} else {
			messages.emplace_back(boost::json::object{{"role", role}, {"content", std::move(content)}});
		}
	}
	flush_system_updates();

	std::string effort = options.reasoning_effort;
	if (!effort.empty() && !valid_effort(effort))
		throw llm_error(
			llm_failure{llm_error_code::invalid_request, "Messages does not support reasoning effort " + effort});
	if (effort.empty())
		effort = model.default_reasoning_effort.value_or("high");
	if (!valid_effort(effort))
		throw llm_error(
			llm_failure{llm_error_code::invalid_request, "Messages does not support reasoning effort " + effort});

	std::uint64_t const max_tokens = options.max_tokens				 ? *options.max_tokens
									 : model.default_max_tokens != 0 ? model.default_max_tokens
																	 : config.fallback_max_tokens;

	std::string system = options.system;
	if (!history_system.empty()) {
		if (!system.empty())
			system += "\n\n";
		system += history_system;
	}

	boost::json::object root;
	root["model"] = options.model;
	root["stream"] = true;
	root["max_tokens"] = max_tokens;
	root["messages"] = std::move(messages);
	if (!system.empty())
		root["system"] = std::move(system);
	if (config.thinking_budget_tokens > 0) {
		// Anthropic-native extended thinking.
		boost::json::object thinking;
		thinking["type"] = effort == "off" ? "disabled" : "enabled";
		if (effort != "off")
			thinking["budget_tokens"] = config.thinking_budget_tokens;
		root["thinking"] = std::move(thinking);
	} else {
		root["thinking"] = boost::json::object{{"type", effort == "off" ? "disabled" : "enabled"}};
		if (effort != "off")
			root["output_config"] = boost::json::object{{"effort", effort}};
	}
	if (options.temperature)
		root["temperature"] = *options.temperature;
	if (!options.stop.empty()) {
		boost::json::array stop;
		for (auto const& sequence : options.stop)
			stop.emplace_back(sequence);
		root["stop_sequences"] = std::move(stop);
	}
	if (!options.tools.empty()) {
		boost::json::array tools;
		for (auto const& tool : options.tools) {
			tools.emplace_back(boost::json::object{
				{"name", tool.name}, {"description", tool.description}, {"input_schema", tool.parameters}});
		}
		root["tools"] = std::move(tools);
	}
	return root;
}

llm_failure messages_failure_for(
	unsigned status,
	std::string_view body,
	std::optional<std::chrono::milliseconds> retry_after,
	std::string request_id) {
	std::string message;
	std::string type;
	try {
		auto const parsed = boost::json::parse(body);
		if (auto const* error = parsed.as_object().if_contains("error"); error && error->is_object()) {
			auto const& object = error->as_object();
			if (auto const* node = object.if_contains("type"); node && node->is_string())
				type = std::string(node->as_string());
			if (auto const* node = object.if_contains("message"); node && node->is_string())
				message = std::string(node->as_string());
		}
	} catch (...) {
		// The HTTP status stays authoritative on malformed JSON bodies.
	}

	std::string const detail = (type + " " + message);
	auto contains = [&detail](std::string_view needle) { return detail.find(needle) != std::string::npos; };

	llm_error_code code = llm_error_code::invalid_request;
	if (status == 401 || status == 403 || type == "authentication_error" || type == "permission_error")
		code = llm_error_code::auth;
	else if (status == 402 || contains("quota"))
		code = llm_error_code::quota;
	else if (status == 429 || type == "rate_limit_error")
		code = llm_error_code::rate_limit;
	else if (contains("context") && (contains("exceed") || contains("length") || contains("window")))
		code = llm_error_code::context_window_exceeded;
	else if (status == 400 || status == 413 || type == "invalid_request_error")
		code = llm_error_code::invalid_request;
	else if (status >= 500 || type == "api_error" || type == "overloaded_error")
		code = llm_error_code::server;

	llm_failure failure{
		code, message.empty() ? "provider error (HTTP " + std::to_string(status) + ")" : std::move(message)};
	failure.status = static_cast<int>(status);
	failure.provider_retry_after = retry_after;
	failure.request_id = std::move(request_id);
	failure.provider_code = std::move(type);
	return failure;
}

messages_translator::messages_translator(std::string model)
	: model_(std::move(model)) {}

messages_translator::open_block&
messages_translator::start_block(boost::json::value const& content_block, std::vector<stream_chunk>& chunks) {
	auto const* object = content_block.if_object();
	if (!object)
		throw llm_error(malformed("content_block_start is not an object"));
	std::string const type = araya::util::json::get_string(*object, "type");
	auto& block = order_.emplace_back();
	block.index = order_.size() - 1;
	if (type == "text") {
		block.kind = content_block_type::text;
		block.text = araya::util::json::get_string(*object, "text");
	} else if (type == "thinking") {
		block.kind = content_block_type::reasoning;
		block.text = araya::util::json::get_string(*object, "thinking");
		if (auto const* signature = object->if_contains("signature"); signature && signature->is_string())
			block.signature = std::string(signature->as_string());
	} else if (type == "tool_use") {
		block.kind = content_block_type::tool_call;
		block.call_id = araya::util::json::get_string(*object, "id");
		block.name = araya::util::json::get_string(*object, "name");
		if (auto const* input = object->if_contains("input"); input && input->is_object())
			block.text = boost::json::serialize(*input);
	} else {
		throw llm_error(malformed("unsupported content block " + type));
	}
	chunks.emplace_back(araya::llm::block_start_chunk{block.index, block.kind});
	if (block.kind == content_block_type::text && !block.text.empty())
		chunks.emplace_back(araya::llm::text_delta_chunk{block.index, block.text});
	else if (block.kind == content_block_type::reasoning && !block.text.empty())
		chunks.emplace_back(araya::llm::reasoning_delta_chunk{block.index, block.text});
	else if (block.kind == content_block_type::tool_call) {
		araya::llm::tool_call_delta_chunk delta;
		delta.index = block.index;
		delta.id = block.call_id;
		delta.name = block.name;
		chunks.emplace_back(std::move(delta));
	}
	return block;
}

std::vector<stream_chunk> messages_translator::delta(open_block& block, boost::json::value const& value) {
	std::vector<stream_chunk> chunks;
	auto const* object = value.if_object();
	if (!object)
		throw llm_error(malformed("content_block_delta is not an object"));
	std::string const type = araya::util::json::get_string(*object, "type");
	if (type == "text_delta" && block.kind == content_block_type::text) {
		std::string const text = araya::util::json::get_string(*object, "text");
		block.text += text;
		chunks.emplace_back(araya::llm::text_delta_chunk{block.index, text});
	} else if (type == "thinking_delta" && block.kind == content_block_type::reasoning) {
		std::string const text = araya::util::json::get_string(*object, "thinking");
		block.text += text;
		chunks.emplace_back(araya::llm::reasoning_delta_chunk{block.index, text});
	} else if (type == "signature_delta" && block.kind == content_block_type::reasoning) {
		block.signature += araya::util::json::get_string(*object, "signature");
	} else if (type == "input_json_delta" && block.kind == content_block_type::tool_call) {
		std::string const fragment = araya::util::json::get_string(*object, "partial_json");
		block.json += fragment;
		araya::llm::tool_call_delta_chunk delta_chunk;
		delta_chunk.index = block.index;
		delta_chunk.id = block.call_id;
		delta_chunk.name = block.name;
		delta_chunk.arguments_delta = fragment;
		chunks.emplace_back(std::move(delta_chunk));
	} else {
		throw llm_error(malformed("unsupported delta " + type));
	}
	return chunks;
}

void messages_translator::stop_block(open_block& block, std::vector<stream_chunk>& chunks) {
	block.closed = true;
	araya::llm::content_block content;
	switch (block.kind) {
	case content_block_type::text:
		content = araya::llm::text_block{block.text};
		break;
	case content_block_type::reasoning:
		content = araya::llm::reasoning_block{block.text};
		break;
	case content_block_type::tool_call: {
		std::string const arguments = block.json.empty() ? block.text : block.json;
		content = araya::llm::tool_call_block{block.call_id, block.name, arguments};
		break;
	}
	case content_block_type::tool_result:
		return;
	}
	chunks.emplace_back(araya::llm::block_end_chunk{block.index, std::move(content)});
}

boost::json::value messages_translator::replay_state() const {
	boost::json::array signatures;
	for (auto const& block : order_)
		signatures.push_back(
			block.kind == content_block_type::reasoning ? boost::json::value(block.signature) : boost::json::value{});
	return boost::json::object{{"format", "messages"}, {"model", model_}, {"signatures", std::move(signatures)}};
}

std::vector<stream_chunk> messages_translator::feed(boost::json::value const& event) {
	if (done_)
		throw llm_error(malformed("event after message_stop"));
	auto const* object = event.if_object();
	if (!object)
		throw llm_error(malformed("event is not an object"));
	std::string const type = araya::util::json::get_string(*object, "type");
	std::vector<stream_chunk> chunks;

	if (type == "message_start") {
		if (started_)
			throw llm_error(malformed("duplicate message_start"));
		if (auto const* message = object->if_contains("message"); message && message->is_object()) {
			if (auto const* usage = message->as_object().if_contains("usage"); usage && usage->is_object()) {
				auto const& fields = usage->as_object();
				usage_.input_tokens = araya::util::json::get_uint(fields, "input_tokens");
				usage_.output_tokens = araya::util::json::get_uint(fields, "output_tokens");
				usage_.cache_read_tokens = araya::util::json::opt_uint(fields, "cache_read_input_tokens");
				usage_.cache_write_tokens = araya::util::json::opt_uint(fields, "cache_creation_input_tokens");
			}
		}
		started_ = true;
		return chunks;
	}

	if (type == "content_block_start") {
		if (!started_)
			throw llm_error(malformed("event precedes message_start"));
		auto const* index_node = object->if_contains("index");
		if (!index_node || !index_node->is_int64() || index_node->as_int64() < 0)
			throw llm_error(malformed("invalid block index"));
		auto const index = static_cast<std::size_t>(index_node->as_int64());
		if (wire_index_.contains(index) || pending_finish_)
			throw llm_error(malformed("block start repeats an index or follows settlement"));
		auto const* content = object->if_contains("content_block");
		if (!content)
			throw llm_error(malformed("content_block_start has no content_block"));
		wire_index_[index] = order_.size();
		start_block(*content, chunks);
		return chunks;
	}

	auto const* index_node = object->if_contains("index");
	std::size_t index = 0;
	if (index_node) {
		if (!index_node->is_int64() || index_node->as_int64() < 0)
			throw llm_error(malformed("invalid block index"));
		index = static_cast<std::size_t>(index_node->as_int64());
	}

	if (type == "content_block_delta") {
		auto const found = wire_index_.find(index);
		if (found == wire_index_.end())
			throw llm_error(malformed("delta without an open block"));
		auto& block = order_[found->second];
		if (block.closed)
			throw llm_error(malformed("delta on a closed block"));
		auto const* value = object->if_contains("delta");
		if (!value)
			throw llm_error(malformed("content_block_delta has no delta"));
		return delta(block, *value);
	}

	if (type == "content_block_stop") {
		auto const found = wire_index_.find(index);
		if (found == wire_index_.end())
			throw llm_error(malformed("stop without an open block"));
		auto& block = order_[found->second];
		if (block.closed)
			throw llm_error(malformed("stop on a closed block"));
		stop_block(block, chunks);
		return chunks;
	}

	if (type == "message_delta") {
		if (auto const* value = object->if_contains("delta"); value && value->is_object()) {
			std::string const reason = araya::util::json::get_string(value->as_object(), "stop_reason");
			if (!reason.empty()) {
				if (reason == "end_turn" || reason == "stop_sequence")
					pending_finish_ = finish_chunk::reason::stop;
				else if (reason == "tool_use")
					pending_finish_ = finish_chunk::reason::tool_calls;
				else if (reason == "max_tokens")
					pending_finish_ = finish_chunk::reason::max_tokens;
				else {
					pending_finish_ = finish_chunk::reason::error;
					llm_failure failure{llm_error_code::malformed_response, "model stopped: " + reason};
					failure.provider_code = reason;
					pending_failure_ = std::move(failure);
				}
			}
		}
		if (auto const* usage = object->if_contains("usage"); usage && usage->is_object()) {
			if (auto const* output = usage->as_object().if_contains("output_tokens"); output && !output->is_null())
				usage_.output_tokens = araya::util::json::get_uint(*output);
			if (auto const* input = usage->as_object().if_contains("input_tokens"); input && !input->is_null())
				usage_.input_tokens = araya::util::json::get_uint(*input);
		}
		return chunks;
	}

	if (type == "message_stop") {
		bool const settled =
			std::all_of(order_.begin(), order_.end(), [](open_block const& block) { return block.closed; });
		if (!settled || !pending_finish_)
			throw llm_error(malformed("message_stop before settled blocks and a stop reason"));
		auto const reason = *pending_finish_;
		if (order_.empty() && reason == finish_chunk::reason::stop) {
			chunks.emplace_back(finish_from(
				llm_failure{llm_error_code::empty_response, "model returned a completed response with no content"}));
			done_ = true;
			return chunks;
		}
		usage_.total_tokens = usage_.input_tokens + usage_.output_tokens + usage_.cache_read_tokens.value_or(0) +
							  usage_.cache_write_tokens.value_or(0);
		chunks.emplace_back(araya::llm::usage_chunk{usage_});
		finish_chunk finish;
		finish.why = reason;
		finish.failure = pending_failure_;
		finish.replay_state = replay_state();
		chunks.emplace_back(std::move(finish));
		done_ = true;
		return chunks;
	}

	// Anthropic permits additional event types; ignore the rest.
	return chunks;
}

araya::task<void> run_messages_call(
	boost::asio::any_io_executor executor,
	compat_config const& config,
	compat_model const& model,
	araya::llm::generate_options const& options,
	araya::llm::chunk_sink const& sink,
	araya::logger::log_handle log) {
	log_request_start(log, config.provider, options.model, "messages", config.base_url);
	std::optional<finish_chunk> terminal;
	try {
		auto endpoint = araya::llm::http::parse_url(config.base_url);
		araya::llm::http::request request;
		request.server = std::move(endpoint);
		request.target = request.server.path + "/v1/messages";
		request.method = boost::beast::http::verb::post;
		request.body = boost::json::serialize(build_messages_request(options, config, model));
		if (char const* key = std::getenv(config.api_key_env.c_str()); key && *key)
			request.headers.emplace_back("x-api-key", key);
		request.headers.emplace_back("anthropic-version", "2023-06-01");
		request.headers.emplace_back("accept", "text/event-stream");

		araya::llm::http::request_options request_options;
		request_options.connect_timeout = config.connect_timeout;
		request_options.idle_timeout = config.idle_timeout;
		request_options.verify_peer = config.verify_peer;

		araya::llm::sse_parser parser;
		messages_translator translator(options.model);
		std::deque<stream_chunk> pending;
		parser.on_event = [&](std::string_view, std::string_view data) {
			if (translator.done())
				throw llm_error(malformed("data after message_stop"));
			boost::json::value wire;
			try {
				wire = boost::json::parse(data);
			} catch (...) {
				throw llm_error(malformed("malformed SSE payload"));
			}
			for (auto& chunk : translator.feed(wire))
				pending.push_back(std::move(chunk));
		};
		auto on_body = [&](std::string_view bytes) -> araya::task<void> {
			parser.feed(bytes);
			while (!pending.empty()) {
				auto chunk = std::move(pending.front());
				pending.pop_front();
				co_await sink(std::move(chunk));
			}
			co_return;
		};

		auto const response =
			co_await araya::llm::http::stream_request(executor, request, on_body, request_options, options.stop_token);
		if (response.status < 200 || response.status >= 300) {
			terminal = finish_from(messages_failure_for(
				response.status,
				response.body,
				parse_retry_after(araya::llm::http::header_value(response.headers, "retry-after")),
				araya::llm::http::header_value(response.headers, "x-request-id")));
		} else if (!translator.done()) {
			terminal =
				finish_from(llm_failure{llm_error_code::stream_closed, "Messages stream ended before message_stop"});
		} else {
			while (!pending.empty()) {
				auto chunk = std::move(pending.front());
				pending.pop_front();
				co_await sink(std::move(chunk));
			}
		}
	} catch (llm_error const& e) {
		terminal = finish_from(e.failure());
	} catch (boost::system::system_error const& e) {
		terminal = finish_from(llm_failure{llm_error_code::transport, e.what()});
	} catch (std::exception const& e) {
		terminal = finish_from(llm_failure{llm_error_code::invalid_request, e.what()});
	}
	if (terminal && terminal->failure)
		log_request_failure(log, config.provider, options.model, *terminal->failure);
	if (terminal)
		co_await sink(std::move(*terminal));
}

} // namespace araya::llm_compat
