#include "araya/session/surface.hpp"

#include <boost/json.hpp>

namespace araya::session {

bool is_builtin_surface_type(std::string_view type) noexcept {
    return type == "system/message" || type == "user/message" ||
           type == "assistant/message" || type == "tool/result";
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

std::optional<std::string> string_field(boost::json::object const& obj,
                                        std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->value().is_string())
        return std::nullopt;
    return std::string(it->value().as_string());
}

std::optional<session_message> fold_builtin(session_event const& ev) {
    if (ev.type == "user/message") {
        auto const* obj = message_object(ev);
        if (!obj)
            return std::nullopt;
        auto id = string_field(*obj, "id");
        auto it = obj->find("content");
        if (!id || it == obj->end())
            return std::nullopt;
        return session_message{message_role::user, *std::move(id),
                               it->value(), std::nullopt, std::nullopt};
    }
    if (ev.type == "assistant/message") {
        auto const* obj = message_object(ev);
        if (!obj)
            return std::nullopt;
        auto id = string_field(*obj, "id");
        auto it = obj->find("content");
        if (!id || it == obj->end())
            return std::nullopt;
        return session_message{message_role::assistant, *std::move(id),
                               it->value(), std::nullopt, std::nullopt};
    }
    if (ev.type == "system/message") {
        auto const* obj = message_object(ev);
        if (!obj)
            return std::nullopt;
        auto id = string_field(*obj, "id");
        auto it = obj->find("content");
        if (!id || it == obj->end())
            return std::nullopt;
        std::optional<std::string> plugin;
        if (auto sit = obj->find("source"); sit != obj->end()) {
            if (auto const* src = sit->value().if_object())
                plugin = string_field(*src, "plugin");
        }
        return session_message{message_role::system, *std::move(id),
                               it->value(), std::nullopt, std::move(plugin)};
    }
    if (ev.type == "tool/result") {
        auto const* obj = message_object(ev);
        if (!obj)
            return std::nullopt;
        auto id = string_field(*obj, "id");
        auto it = obj->find("content");
        if (!id || it == obj->end())
            return std::nullopt;
        auto const* content = it->value().if_array();
        if (!content || content->empty())
            return std::nullopt;
        auto const* block = (*content)[0].if_object();
        if (!block)
            return std::nullopt;
        auto call_id = string_field(*block, "tool_call_id");
        if (!call_id)
            return std::nullopt;
        return session_message{message_role::tool_result, *std::move(id),
                               it->value(), std::move(call_id), std::nullopt};
    }
    return std::nullopt;
}

}  // namespace

std::optional<session_message> fold_event(
    session_event const& ev,
    std::vector<message_projection> const& projections) {
    for (auto const& p : projections) {
        if (p.event_type == ev.type)
            return p.fold(ev);
    }
    return fold_builtin(ev);
}

}  // namespace araya::session
