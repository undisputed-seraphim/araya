#include "araya/session/surface.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <utility>

namespace araya::session {

bool is_builtin_surface_type(std::string_view type) noexcept {
	return type == "system/message" || type == "user/message" || type == "assistant/message" || type == "tool/result" ||
		   type == "surface/replace";
}

void session_surface::push(session_seq source_seq, session_message msg) {
	messages_.push_back(std::move(msg));
	source_seqs_.push_back(source_seq);
}

void session_surface::replace(
	session_seq start,
	session_seq end,
	session_seq source_seq,
	std::optional<session_message> msg) {
	std::size_t write = 0;
	for (std::size_t i = 0; i < messages_.size(); ++i) {
		if (source_seqs_[i] >= start && source_seqs_[i] < end)
			continue;
		messages_[write] = std::move(messages_[i]);
		source_seqs_[write] = source_seqs_[i];
		++write;
	}
	messages_.resize(write);
	source_seqs_.resize(write);
	if (msg) {
		messages_.push_back(std::move(*msg));
		source_seqs_.push_back(source_seq);
	}
}

namespace {

boost::json::object const* message_object(session_event const& ev) {
	// user/message and tool/result carry the message directly; the other
	// built-ins wrap it under "message". Malformed shapes are the
	// validator's concern: the fold tolerates and folds nothing.
	auto const* obj = ev.data.if_object();
	if (!obj)
		return nullptr;
	if (ev.type == "user/message" || ev.type == "tool/result")
		return obj;
	auto it = obj->find("message");
	if (it == obj->end())
		return nullptr;
	return it->value().if_object();
}

std::optional<std::string> string_field(boost::json::object const& obj, std::string_view key) {
	auto it = obj.find(key);
	if (it == obj.end() || !it->value().is_string())
		return std::nullopt;
	return std::string(it->value().as_string());
}

std::optional<message_role> parse_role(std::string_view role) {
	if (role == "user")
		return message_role::user;
	if (role == "assistant")
		return message_role::assistant;
	if (role == "system")
		return message_role::system;
	if (role == "tool_result")
		return message_role::tool_result;
	return std::nullopt;
}

surface_plan append_plan(std::optional<session_message> msg) {
	surface_plan plan;
	if (!msg)
		return plan;
	plan.action = surface_plan::kind::append;
	plan.message = std::move(msg);
	return plan;
}

surface_plan fold_builtin(session_event const& ev) {
	if (ev.type == "user/message") {
		auto const* obj = message_object(ev);
		if (!obj)
			return {};
		auto id = string_field(*obj, "id");
		auto it = obj->find("content");
		if (!id || it == obj->end())
			return {};
		return append_plan(
			session_message{message_role::user, *std::move(id), it->value(), std::nullopt, std::nullopt});
	}
	if (ev.type == "assistant/message") {
		auto const* obj = message_object(ev);
		if (!obj)
			return {};
		auto id = string_field(*obj, "id");
		auto it = obj->find("content");
		if (!id || it == obj->end())
			return {};
		return append_plan(
			session_message{message_role::assistant, *std::move(id), it->value(), std::nullopt, std::nullopt});
	}
	if (ev.type == "system/message") {
		auto const* obj = message_object(ev);
		if (!obj)
			return {};
		auto id = string_field(*obj, "id");
		auto it = obj->find("content");
		if (!id || it == obj->end())
			return {};
		std::optional<std::string> plugin;
		if (auto sit = obj->find("source"); sit != obj->end()) {
			if (auto const* src = sit->value().if_object())
				plugin = string_field(*src, "plugin");
		}
		return append_plan(
			session_message{message_role::system, *std::move(id), it->value(), std::nullopt, std::move(plugin)});
	}
	if (ev.type == "tool/result") {
		auto const* obj = message_object(ev);
		if (!obj)
			return {};
		auto id = string_field(*obj, "id");
		auto it = obj->find("content");
		if (!id || it == obj->end())
			return {};
		auto const* content = it->value().if_array();
		if (!content || content->empty())
			return {};
		auto const* block = (*content)[0].if_object();
		if (!block)
			return {};
		auto call_id = string_field(*block, "tool_call_id");
		if (!call_id)
			return {};
		return append_plan(
			session_message{message_role::tool_result, *std::move(id), it->value(), std::move(call_id), std::nullopt});
	}
	if (ev.type == "surface/replace") {
		auto const* obj = ev.data.if_object();
		if (!obj)
			return {};
		auto start = obj->find("start_seq");
		auto end = obj->find("end_seq");
		if (start == obj->end() || end == obj->end() || !start->value().is_int64() || !end->value().is_int64())
			return {};
		surface_plan plan;
		plan.action = surface_plan::kind::replace;
		plan.start = static_cast<session_seq>(start->value().as_int64());
		plan.end = static_cast<session_seq>(end->value().as_int64());
		if (auto mit = obj->find("message"); mit != obj->end()) {
			auto const* msg = mit->value().if_object();
			if (msg) {
				auto id = string_field(*msg, "id");
				auto role = string_field(*msg, "role");
				auto content = msg->find("content");
				if (id && role && content != msg->end()) {
					if (auto parsed = parse_role(*role))
						plan.message =
							session_message{*parsed, *std::move(id), content->value(), std::nullopt, std::nullopt};
				}
			}
		}
		return plan;
	}
	return {};
}

} // namespace

surface_plan fold_event(session_event const& ev, std::vector<message_projection> const& projections) {
	for (auto const& p : projections) {
		if (p.event_type == ev.type) {
			surface_plan plan;
			plan.action = surface_plan::kind::append;
			plan.message = p.fold(ev);
			if (!plan.message)
				plan.action = surface_plan::kind::none;
			return plan;
		}
	}
	return fold_builtin(ev);
}

} // namespace araya::session
