#pragma once

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// A minimal JSON-RPC 2.0 codec for the MCP wire: request/notification
// builders and an inbound-message view. Only the shapes the client needs are
// understood; unsupported inbound requests (server -> client) are ignored.
namespace araya::mcp::jsonrpc {

inline boost::json::value make_request(std::int64_t id, std::string_view method, boost::json::value params) {
	boost::json::object message;
	message["jsonrpc"] = "2.0";
	message["id"] = id;
	message["method"] = method;
	if (!params.is_null())
		message["params"] = std::move(params);
	return message;
}

inline boost::json::value make_notification(std::string_view method, boost::json::value params) {
	boost::json::object message;
	message["jsonrpc"] = "2.0";
	message["method"] = method;
	if (!params.is_null())
		message["params"] = std::move(params);
	return message;
}

// One decoded inbound frame. `id` is the serialized id token (integer or
// string) used to correlate a response with its pending request.
struct inbound {
	bool has_id = false;
	std::string id;
	std::string method;
	bool has_method = false;
	bool is_error = false;
	boost::json::value result;
	boost::json::value error;
	boost::json::value params;
};

// Decodes one frame. Returns nullopt for a non-object payload or a payload
// that is neither a response nor a notification.
inline std::optional<inbound> decode(std::string_view text) {
	boost::json::value parsed;
	try {
		parsed = boost::json::parse(text);
	} catch (std::exception const&) {
		return std::nullopt;
	}
	auto const* object = parsed.if_object();
	if (!object)
		return std::nullopt;
	inbound message;
	if (auto const* id = object->if_contains("id"); id && !id->is_null()) {
		message.has_id = true;
		message.id = boost::json::serialize(*id);
	}
	if (auto const* method = object->if_contains("method"); method && method->is_string()) {
		message.has_method = true;
		message.method = std::string(method->as_string());
	}
	if (auto const* error = object->if_contains("error")) {
		message.is_error = true;
		message.error = *error;
	}
	if (auto const* result = object->if_contains("result"))
		message.result = *result;
	if (auto const* params = object->if_contains("params"))
		message.params = *params;
	if (!message.has_id && !message.has_method)
		return std::nullopt;
	return message;
}

// A short human-readable rendering of a JSON-RPC error object.
inline std::string error_text(boost::json::value const& error) {
	if (auto const* object = error.if_object()) {
		auto const* message = object->if_contains("message");
		auto const* code = object->if_contains("code");
		std::string text;
		if (code) {
			if (code->is_int64())
				text = std::to_string(code->as_int64());
			else if (code->is_uint64())
				text = std::to_string(code->as_uint64());
		}
		if (message && message->is_string()) {
			if (!text.empty())
				text += ": ";
			text += std::string(message->as_string());
		}
		if (!text.empty())
			return text;
	}
	return boost::json::serialize(error);
}

} // namespace araya::mcp::jsonrpc
