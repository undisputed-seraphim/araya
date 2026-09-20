#include "translate.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace araya::llm_openai {
namespace {

using araya::llm::content_block_type;

std::string join_text(araya::llm::llm_message const& message) {
	std::string text;
	for (auto const& block : message.content) {
		if (auto const* t = std::get_if<araya::llm::text_block>(&block))
			text += t->text;
	}
	return text;
}

bool has_reasoning_effort(std::string_view effort) { return effort == "low" || effort == "high" || effort == "max"; }

bool contains(std::string_view haystack, std::string_view needle) {
	return haystack.find(needle) != std::string_view::npos;
}

bool looks_like_quota(std::string_view detail) { return contains(detail, "quota"); }

bool looks_like_context_window(std::string_view detail) {
	return contains(detail, "context") && (contains(detail, "exceed") || contains(detail, "length") ||
										   contains(detail, "window") || contains(detail, "maximum"));
}

token_usage map_usage(boost::json::value const& usage) {
	auto const& object = usage.as_object();
	auto const prompt = object.at("prompt_tokens").to_number<std::uint64_t>();
	auto const completion = object.at("completion_tokens").to_number<std::uint64_t>();

	token_usage result;
	result.input_tokens = prompt;
	result.output_tokens = completion;
	result.total_tokens = prompt + completion;

	std::optional<std::uint64_t> cache_read;
	if (auto const details = object.if_contains("prompt_tokens_details"); details && details->is_object()) {
		if (auto const cached = details->as_object().if_contains("cached_tokens"))
			cache_read = cached->to_number<std::uint64_t>();
	}
	if (!cache_read) {
		if (auto const legacy = object.if_contains("prompt_cache_hit_tokens"))
			cache_read = legacy->to_number<std::uint64_t>();
	}
	if (cache_read) {
		result.cache_read_tokens = *cache_read;
		result.input_tokens = prompt > *cache_read ? prompt - *cache_read : 0;
	}
	if (auto const details = object.if_contains("completion_tokens_details"); details && details->is_object()) {
		if (auto const reasoning = details->as_object().if_contains("reasoning_tokens"))
			result.reasoning_tokens = reasoning->to_number<std::uint64_t>();
	}
	return result;
}

// Identity fields on tool-call deltas: the wire sends each once; empty or
// missing continuations mean "no update", never "clear".
std::string accept_identity(std::string const& current, boost::json::value const* incoming) {
	if (incoming && incoming->is_string() && !incoming->as_string().empty())
		return std::string(incoming->as_string());
	return current;
}

} // namespace

std::optional<std::chrono::milliseconds> parse_retry_after(std::string_view value) {
	if (value.empty())
		return std::nullopt;
	// Seconds first, then the HTTP-date form.
	auto const all_digits =
		!value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
	if (all_digits) {
		auto const seconds = std::strtoll(std::string(value).c_str(), nullptr, 10);
		if (seconds >= 0)
			return std::chrono::seconds(seconds);
	}
	std::tm tm{};
	std::istringstream is{std::string(value)};
	is >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
	if (is.fail())
		return std::nullopt;
	auto const point = std::chrono::system_clock::from_time_t(std::mktime(&tm));
	auto const delay = point - std::chrono::system_clock::now();
	if (delay <= std::chrono::seconds(0))
		return std::chrono::milliseconds(0);
	return std::chrono::duration_cast<std::chrono::milliseconds>(delay);
}

boost::json::value build_request(generate_options const& options) {
	boost::json::object root;
	root["model"] = options.model;

	boost::json::array messages;
	if (options.system)
		messages.emplace_back(boost::json::object{{"role", "system"}, {"content", *options.system}});
	for (auto const& message : options.messages) {
		switch (message.role) {
		case araya::llm::message_role::system:
			messages.emplace_back(boost::json::object{{"role", "system"}, {"content", join_text(message)}});
			break;
		case araya::llm::message_role::user: {
			// Tool results serialize as role "tool" messages first; the
			// remaining text becomes the user message.
			for (auto const& block : message.content) {
				if (auto const* tool = std::get_if<araya::llm::tool_result_block>(&block)) {
					boost::json::object entry;
					entry["role"] = "tool";
					entry["tool_call_id"] = tool->tool_call_id;
					std::string text;
					if (tool->content.is_array()) {
						for (auto const& inner : tool->content.as_array()) {
							if (auto const* text_node = inner.if_object()) {
								if (auto const* type = text_node->if_contains("type");
									type && type->is_string() && type->as_string() == "text") {
									if (auto const* node = text_node->if_contains("text"); node && node->is_string())
										text += node->as_string();
								}
							}
						}
					}
					entry["content"] = text;
					messages.emplace_back(std::move(entry));
				}
			}
			auto text = join_text(message);
			if (!text.empty())
				messages.emplace_back(boost::json::object{{"role", "user"}, {"content", std::move(text)}});
			break;
		}
		case araya::llm::message_role::assistant: {
			boost::json::object entry;
			entry["role"] = "assistant";
			boost::json::array tool_calls;
			for (auto const& block : message.content) {
				if (auto const* call = std::get_if<araya::llm::tool_call_block>(&block)) {
					tool_calls.emplace_back(boost::json::object{
						{"id", call->id},
						{"type", "function"},
						{"function", boost::json::object{{"name", call->name}, {"arguments", call->arguments}}}});
				}
			}
			if (!tool_calls.empty())
				entry["tool_calls"] = std::move(tool_calls);
			auto text = join_text(message);
			if (!text.empty())
				entry["content"] = std::move(text);
			if (entry.contains("tool_calls") || entry.contains("content"))
				messages.emplace_back(std::move(entry));
			break;
		}
		}
	}
	root["messages"] = std::move(messages);
	root["stream"] = true;
	root["stream_options"] = boost::json::value{{"include_usage", true}};
	if (options.reasoning_effort && has_reasoning_effort(*options.reasoning_effort)) {
		root["thinking"] = boost::json::object{{"type", "enabled"}};
		root["reasoning_effort"] = *options.reasoning_effort;
	}
	if (!options.tools.empty()) {
		boost::json::array tools;
		for (auto const& tool : options.tools) {
			tools.emplace_back(boost::json::object{
				{"type", "function"},
				{"function",
				 boost::json::object{
					 {"name", tool.name}, {"description", tool.description}, {"parameters", tool.parameters}}}});
		}
		root["tools"] = std::move(tools);
	}
	if (options.temperature)
		root["temperature"] = *options.temperature;
	if (options.max_tokens)
		root["max_tokens"] = *options.max_tokens;
	if (!options.stop.empty()) {
		boost::json::array stop;
		for (auto const& sequence : options.stop)
			stop.emplace_back(sequence);
		root["stop"] = std::move(stop);
	}
	return root;
}

llm_failure failure_for(
	unsigned status,
	std::string_view body,
	std::optional<std::chrono::milliseconds> retry_after,
	std::optional<std::string> request_id) {
	std::string message;
	std::string detail;
	std::string provider_code;
	try {
		auto const parsed = boost::json::parse(body);
		if (auto const* error = parsed.as_object().if_contains("error"); error && error->is_object()) {
			auto const& object = error->as_object();
			// The provider's verbatim machine code: error.code wins, then
			// error.type. Diagnostic only - classification below stays on
			// the HTTP status and the folded detail text.
			if (auto const* node = object.if_contains("code"); node && node->is_string() && !node->as_string().empty())
				provider_code = std::string(node->as_string());
			if (provider_code.empty()) {
				if (auto const* node = object.if_contains("type"); node && node->is_string())
					provider_code = std::string(node->as_string());
			}
			for (auto const* field : {"code", "type", "message"}) {
				if (auto const* node = object.if_contains(field); node && node->is_string()) {
					if (!detail.empty())
						detail += ' ';
					detail += node->as_string();
					if (std::string_view(field) == "message")
						message = std::string(node->as_string());
				}
			}
		}
	} catch (...) {
		// The HTTP status stays authoritative on malformed JSON bodies.
	}

	llm_error_code code = llm_error_code::invalid_request;
	if (status == 401 || status == 403) {
		code = llm_error_code::auth;
	} else if (status == 413) {
		code = llm_error_code::invalid_request;
	} else if (looks_like_quota(detail)) {
		code = llm_error_code::quota;
	} else if (status == 429) {
		code = llm_error_code::rate_limit;
	} else if (status == 400) {
		code = looks_like_context_window(detail) ? llm_error_code::context_window_exceeded
												 : llm_error_code::invalid_request;
	} else if (status >= 500) {
		code = llm_error_code::server;
	}

	llm_failure failure{
		code, message.empty() ? "provider error (HTTP " + std::to_string(status) + ")" : std::move(message)};
	failure.status = status;
	failure.provider_retry_after = retry_after;
	failure.request_id = request_id;
	failure.provider_code = std::move(provider_code);
	return failure;
}

chunk_translator::open_block& chunk_translator::open(araya::llm::content_block_type kind) {
	auto& block = order_.emplace_back();
	block.index = next_index_++;
	block.kind = kind;
	return block;
}

std::vector<stream_chunk> chunk_translator::feed(boost::json::value const& wire) {
	if (done_)
		throw llm_error(llm_failure{llm_error_code::malformed_response, "chunk arrived after [DONE]"});
	std::vector<stream_chunk> chunks;
	if (!wire.is_object())
		throw llm_error(llm_failure{llm_error_code::malformed_response, "SSE payload is not an object"});

	auto const& object = wire.as_object();
	if (auto const* choices = object.if_contains("choices"); choices && choices->is_array()) {
		for (auto const& choice : choices->as_array()) {
			if (!choice.is_object())
				continue;
			auto const& choice_object = choice.as_object();
			auto const* delta_node = choice_object.if_contains("delta");
			if (delta_node && delta_node->is_object()) {
				auto const& delta = delta_node->as_object();

				// Reasoning first: thinking mode interleaves it before
				// text; the empty first chunk must not open a block.
				if (auto const* reasoning = delta.if_contains("reasoning_content");
					reasoning && reasoning->is_string() && !reasoning->as_string().empty()) {
					if (!reasoning_) {
						reasoning_ = &open(content_block_type::reasoning);
						chunks.emplace_back(
							araya::llm::block_start_chunk{reasoning_->index, content_block_type::reasoning});
					}
					reasoning_->text += reasoning->as_string();
					chunks.emplace_back(
						araya::llm::reasoning_delta_chunk{reasoning_->index, std::string(reasoning->as_string())});
				}

				if (auto const* content = delta.if_contains("content");
					content && content->is_string() && !content->as_string().empty()) {
					if (!text_) {
						text_ = &open(content_block_type::text);
						chunks.emplace_back(araya::llm::block_start_chunk{text_->index, content_block_type::text});
					}
					text_->text += content->as_string();
					chunks.emplace_back(araya::llm::text_delta_chunk{text_->index, std::string(content->as_string())});
				}

				if (auto const* calls = delta.if_contains("tool_calls"); calls && calls->is_array()) {
					for (auto const& call : calls->as_array()) {
						if (!call.is_object())
							continue;
						auto const& call_object = call.as_object();
						auto const wire_index =
							call_object.contains("index")
								? static_cast<std::size_t>(call_object.at("index").to_number<std::int64_t>())
								: 0;
						auto const found = tool_position_.find(wire_index);
						open_block* block = nullptr;
						if (found == tool_position_.end()) {
							block = &open(content_block_type::tool_call);
							tool_position_[wire_index] = order_.size() - 1;
							chunks.emplace_back(
								araya::llm::block_start_chunk{block->index, content_block_type::tool_call});
						} else {
							block = &order_[found->second];
						}
						auto const* id_node = call_object.if_contains("id");
						block->call_id = accept_identity(block->call_id, id_node);
						boost::json::value const* name_node = nullptr;
						if (auto const* function = call_object.if_contains("function");
							function && function->is_object())
							name_node = function->as_object().if_contains("name");
						block->name = accept_identity(block->name.value_or(""), name_node);
						if (block->name->empty())
							block->name.reset();
						std::string fragment;
						if (auto const* function = call_object.if_contains("function");
							function && function->is_object()) {
							if (auto const* arguments = function->as_object().if_contains("arguments");
								arguments && arguments->is_string())
								fragment = std::string(arguments->as_string());
						}
						block->text += fragment;
						araya::llm::tool_call_delta_chunk delta_chunk;
						delta_chunk.index = block->index;
						delta_chunk.id = block->call_id;
						delta_chunk.name = block->name;
						delta_chunk.arguments_delta = fragment;
						chunks.emplace_back(std::move(delta_chunk));
					}
				}
			}

			if (auto const* reason = choice_object.if_contains("finish_reason"); reason && reason->is_string()) {
				auto const text = std::string(reason->as_string());
				if (text == "stop") {
					pending_finish_ = finish_chunk::reason::stop;
				} else if (text == "tool_calls") {
					pending_finish_ = finish_chunk::reason::tool_calls;
				} else if (text == "length") {
					pending_finish_ = finish_chunk::reason::max_tokens;
				} else {
					// content_filter, insufficient_system_resource, ...
					// The provider's reason rides verbatim in
					// provider_code; the routing code stays the fixed
					// malformed_response.
					pending_finish_ = finish_chunk::reason::error;
					auto failure = llm_failure{llm_error_code::malformed_response, "model stopped: " + text};
					failure.provider_code = text;
					pending_failure_ = std::move(failure);
				}
			}
		}
	}

	if (auto const* usage = object.if_contains("usage"); usage && usage->is_object())
		pending_usage_ = map_usage(*usage);
	return chunks;
}

std::vector<stream_chunk> chunk_translator::finish() {
	if (done_)
		throw llm_error(llm_failure{llm_error_code::malformed_response, "finish() called twice"});
	done_ = true;
	std::vector<stream_chunk> chunks;
	for (auto const& block : order_) {
		switch (block.kind) {
		case content_block_type::text:
			chunks.emplace_back(araya::llm::block_end_chunk{
				block.index, araya::llm::content_block{araya::llm::text_block{block.text}}});
			break;
		case content_block_type::reasoning:
			chunks.emplace_back(araya::llm::block_end_chunk{
				block.index, araya::llm::content_block{araya::llm::reasoning_block{block.text}}});
			break;
		case content_block_type::tool_call:
			chunks.emplace_back(araya::llm::block_end_chunk{
				block.index,
				araya::llm::content_block{
					araya::llm::tool_call_block{block.call_id, block.name.value_or(""), block.text}}});
			break;
		case content_block_type::tool_result:
			break;
		}
	}
	if (pending_usage_)
		chunks.emplace_back(araya::llm::usage_chunk{*pending_usage_});
	auto const reason = pending_finish_.value_or(finish_chunk::reason::stop);
	finish_chunk finish;
	if (reason == finish_chunk::reason::stop && order_.empty()) {
		finish.why = finish_chunk::reason::error;
		finish.failure =
			llm_failure{llm_error_code::empty_response, "model returned a completed response with no content"};
	} else {
		finish.why = reason;
		finish.failure = pending_failure_;
	}
	chunks.emplace_back(std::move(finish));
	return chunks;
}

} // namespace araya::llm_openai
