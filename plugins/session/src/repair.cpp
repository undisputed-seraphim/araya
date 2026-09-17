#include "repair.hpp"

#include <boost/json.hpp>

#include <set>
#include <string>

namespace araya::session {

void repair_interrupted_turns(std::vector<session_event>& log) {
    // Requested tool calls: every assistant content block carrying a
    // tool_call id without a matching tool/result in the log.
    std::set<std::string> open;
    for (auto const& ev : log) {
        if (ev.type != "assistant/message")
            continue;
        auto const* obj = ev.data.if_object();
        if (!obj)
            continue;
        auto mit = obj->find("message");
        if (mit == obj->end())
            continue;
        auto const* msg = mit->value().if_object();
        if (!msg)
            continue;
        auto cit = msg->find("content");
        if (cit == msg->end())
            continue;
        auto const* content = cit->value().if_array();
        if (!content)
            continue;
        for (auto const& block : *content) {
            auto const* b = block.if_object();
            if (!b)
                continue;
            auto tit = b->find("tool_call");
            if (tit == b->end())
                continue;
            auto const* tc = tit->value().if_object();
            if (!tc)
                continue;
            auto iit = tc->find("id");
            if (iit != tc->end() && iit->value().is_string())
                open.insert(std::string(iit->value().as_string()));
        }
    }
    for (auto const& ev : log) {
        if (ev.type != "tool/result")
            continue;
        auto const* obj = ev.data.if_object();
        if (!obj)
            continue;
        auto cit = obj->find("content");
        if (cit == obj->end())
            continue;
        auto const* content = cit->value().if_array();
        if (!content || content->empty())
            continue;
        auto const* block = (*content)[0].if_object();
        if (!block)
            continue;
        auto tit = block->find("tool_call_id");
        if (tit != block->end() && tit->value().is_string())
            open.erase(std::string(tit->value().as_string()));
    }

    if (open.empty())
        return;

    auto const last_time =
        log.empty() ? now_ms() : log.back().time;
    for (auto const& call_id : open) {
        boost::json::array content;
        content.emplace_back(boost::json::object{
            {"tool_call_id", call_id},
            {"type", "tool_result"},
            {"outcome", "unknown"}});
        boost::json::object data;
        data["id"] = "repair-" + call_id;
        data["role"] = "user";
        data["content"] = std::move(content);
        data["source"] =
            boost::json::object{{"kind", "tool"}, {"call_id", call_id}};
        log.push_back(session_event{static_cast<session_seq>(log.size()),
                                    last_time, "tool/result",
                                    std::move(data), false});
    }
}

}  // namespace araya::session
