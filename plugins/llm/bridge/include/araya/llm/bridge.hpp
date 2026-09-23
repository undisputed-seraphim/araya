#pragma once

#include "araya/llm/llm.hpp"
#include "araya/session/session_types.hpp"
#include "araya/util/json.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The session <-> llm bridge: the contract boundary between the session
// plugin's event JSON and the llm seam's typed messages. It lives under
// the llm plugin as its own header-only INTERFACE target (araya::llm-
// bridge) because it is the only thing that knows both vocabularies;
// adapters, which link araya::llm alone, never see session types.
//
// The session fold reads only the "message" member of the built-in
// envelopes; usage/replay/turn/step ride along as siblings.
namespace araya::llm_bridge {

// The session surface -> llm message bridge. Tool results project onto
// the user role (the session's own projection), and unknown block types
// are dropped.
inline araya::llm::llm_message to_llm_message(araya::session::session_message const& message) {
	araya::llm::llm_message result;
	switch (message.role) {
	case araya::session::message_role::system:
		result.role = araya::llm::message_role::system;
		break;
	case araya::session::message_role::user:
		result.role = araya::llm::message_role::user;
		break;
	case araya::session::message_role::assistant:
		result.role = araya::llm::message_role::assistant;
		break;
	case araya::session::message_role::tool_result:
		result.role = araya::llm::message_role::user;
		break;
	}
	if (auto const* array = araya::util::json::as_array(message.content)) {
		for (auto const& block : *array) {
			auto const* object = araya::util::json::as_object(block);
			if (!object)
				continue;
			auto const type = araya::util::json::get_string(*object, "type");
			if (type == "text") {
				result.content.emplace_back(araya::llm::text_block{araya::util::json::get_string(*object, "text")});
			} else if (type == "reasoning") {
				result.content.emplace_back(
					araya::llm::reasoning_block{araya::util::json::get_string(*object, "text")});
			} else if (type == "tool_call") {
				result.content.emplace_back(araya::llm::tool_call_block{
					araya::util::json::get_string(*object, "id"),
					araya::util::json::get_string(*object, "name"),
					araya::util::json::get_string(*object, "arguments")});
			} else if (type == "tool_result") {
				araya::llm::tool_result_block tool;
				tool.tool_call_id = araya::util::json::get_string(*object, "tool_call_id");
				if (auto const* node = object->if_contains("content"))
					tool.content = *node;
				if (auto const* node = object->if_contains("is_error"); node && node->is_bool())
					tool.is_error = node->as_bool();
				result.content.emplace_back(std::move(tool));
			}
		}
	}
	return result;
}

// The joined text of a surface message's text blocks (the system-prompt
// comparison reads this).
inline std::string message_text(araya::session::session_message const& message) {
	std::string text;
	if (auto const* array = araya::util::json::as_array(message.content)) {
		for (auto const& block : *array) {
			auto const* object = araya::util::json::as_object(block);
			if (!object)
				continue;
			if (araya::util::json::get_string(*object, "type") == "text")
				text += araya::util::json::get_string(*object, "text");
		}
	}
	return text;
}

// llm content blocks -> the session content block array.
inline boost::json::value blocks_to_json(std::vector<araya::llm::content_block> const& blocks) {
	boost::json::array array;
	for (auto const& block : blocks) {
		if (auto const* text = std::get_if<araya::llm::text_block>(&block)) {
			array.emplace_back(boost::json::object{{"type", "text"}, {"text", text->text}});
		} else if (auto const* reasoning = std::get_if<araya::llm::reasoning_block>(&block)) {
			array.emplace_back(boost::json::object{{"type", "reasoning"}, {"text", reasoning->text}});
		} else if (auto const* call = std::get_if<araya::llm::tool_call_block>(&block)) {
			array.emplace_back(boost::json::object{
				{"type", "tool_call"}, {"id", call->id}, {"name", call->name}, {"arguments", call->arguments}});
		} else if (auto const* result = std::get_if<araya::llm::tool_result_block>(&block)) {
			boost::json::object object;
			object["type"] = "tool_result";
			object["tool_call_id"] = result->tool_call_id;
			object["content"] = result->content;
			if (result->is_error)
				object["is_error"] = true;
			array.emplace_back(std::move(object));
		}
	}
	return array;
}

// The assistant/message envelope (built-in surface shape); turn/step are
// the agent loop's step ownership and default to none.
inline boost::json::value assistant_message_data(
	std::string_view id,
	std::vector<araya::llm::content_block> const& blocks,
	araya::llm::token_usage const& usage,
	std::optional<boost::json::value> replay = std::nullopt,
	std::uint64_t turn = 0,
	std::uint64_t step = 0,
	bool interrupted = false) {
	boost::json::object data;
	data["message"] =
		boost::json::object{{"id", std::string(id)}, {"role", "assistant"}, {"content", blocks_to_json(blocks)}};
	boost::json::object usage_object;
	usage_object["input_tokens"] = usage.input_tokens;
	usage_object["output_tokens"] = usage.output_tokens;
	if (usage.total_tokens)
		usage_object["total_tokens"] = *usage.total_tokens;
	if (usage.cache_read_tokens)
		usage_object["cache_read_tokens"] = *usage.cache_read_tokens;
	if (usage.cache_write_tokens)
		usage_object["cache_write_tokens"] = *usage.cache_write_tokens;
	if (usage.reasoning_tokens)
		usage_object["reasoning_tokens"] = *usage.reasoning_tokens;
	data["usage"] = std::move(usage_object);
	if (replay)
		data["replay"] = *replay;
	if (turn)
		data["turn"] = turn;
	if (step)
		data["step"] = step;
	if (interrupted)
		data["interrupted"] = true;
	return data;
}

// The system/message envelope (built-in surface shape: source.kind must
// be "plugin").
inline boost::json::value system_message_data(std::string_view id, std::string_view plugin, std::string_view text) {
	return {
		{"message",
		 boost::json::object{
			 {"id", std::string(id)},
			 {"role", "system"},
			 {"content", boost::json::array{{{"type", "text"}, {"text", std::string(text)}}}},
			 {"source", boost::json::object{{"kind", "plugin"}, {"plugin", std::string(plugin)}}}}}};
}

// The user/message envelope (built-in surface shape).
inline boost::json::value user_message_data(std::string_view id, std::string_view text) {
	return {
		{"id", std::string(id)},
		{"role", "user"},
		{"content", boost::json::array{{{"type", "text"}, {"text", std::string(text)}}}},
	};
}

// The tool/result envelope: the data IS the message, exactly one content
// block carrying the call id, and source.call_id matching it (the
// store's validated shape).
inline boost::json::value
tool_result_data(std::string_view id, std::string_view call_id, boost::json::value const& content, bool is_error) {
	return {
		{"id", std::string(id)},
		{"role", "user"},
		{"content",
		 boost::json::array{
			 {{"type", "tool_result"},
			  {"tool_call_id", std::string(call_id)},
			  {"content", content},
			  {"is_error", is_error}}}},
		{"source", boost::json::object{{"kind", "tool"}, {"call_id", std::string(call_id)}}},
	};
}

} // namespace araya::llm_bridge
