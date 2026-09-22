#include "araya/agent-loop/bridge.hpp"

#include <utility>

namespace araya::agent {

namespace {

std::string take_string(boost::json::object const& object, std::string_view key) {
	std::string text;
	if (auto const* node = object.if_contains(key); node && node->is_string())
		text = std::string(node->as_string());
	return text;
}

} // namespace

araya::llm::llm_message to_llm_message(araya::session::session_message const& message) {
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
	if (auto const* array = message.content.if_array()) {
		for (auto const& block : *array) {
			auto const* object = block.if_object();
			if (!object)
				continue;
			std::string_view type;
			if (auto const* node = object->if_contains("type"); node && node->is_string())
				type = node->as_string();
			if (type == "text") {
				result.content.emplace_back(araya::llm::text_block{take_string(*object, "text")});
			} else if (type == "reasoning") {
				result.content.emplace_back(araya::llm::reasoning_block{take_string(*object, "text")});
			} else if (type == "tool_call") {
				result.content.emplace_back(araya::llm::tool_call_block{
					take_string(*object, "id"), take_string(*object, "name"), take_string(*object, "arguments")});
			} else if (type == "tool_result") {
				araya::llm::tool_result_block tool;
				tool.tool_call_id = take_string(*object, "tool_call_id");
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

boost::json::value blocks_to_json(std::vector<araya::llm::content_block> const& blocks) {
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

boost::json::value assistant_message_data(
	std::string_view id,
	std::vector<araya::llm::content_block> const& blocks,
	araya::llm::token_usage const& usage,
	std::optional<boost::json::value> replay,
	std::uint64_t turn,
	std::uint64_t step,
	bool interrupted) {
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
	data["turn"] = turn;
	data["step"] = step;
	if (interrupted)
		data["interrupted"] = true;
	return data;
}

boost::json::value system_message_data(std::string_view id, std::string_view plugin, std::string_view text) {
	return {
		{"message",
		 boost::json::object{
			 {"id", std::string(id)},
			 {"role", "system"},
			 {"content", boost::json::array{{{"type", "text"}, {"text", std::string(text)}}}},
			 {"source", boost::json::object{{"kind", "plugin"}, {"plugin", std::string(plugin)}}}}}};
}

boost::json::value user_message_data(std::string_view id, std::string_view text) {
	return {
		{"id", std::string(id)},
		{"role", "user"},
		{"content", boost::json::array{{{"type", "text"}, {"text", std::string(text)}}}},
	};
}

boost::json::value
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

} // namespace araya::agent
