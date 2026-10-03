#include "araya/tool-subagent-control/tool_subagent_control.hpp"

#include "araya/subagents/subagents.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace araya::tool_subagent_control {
namespace {

using namespace araya::subagents;
using araya::tools::text_result;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;

araya::task<tool_result> send_message_handler(std::shared_ptr<subagents_service> subs, tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto id = args ? araya::util::json::get_string(*args, "id") : std::string{};
	auto message = args ? araya::util::json::get_string(*args, "message") : std::string{};
	if (id.empty() || message.empty())
		co_return text_result("Error: send_message requires 'id' and 'message'", true);
	auto r = co_await subs->send_message(id, std::move(message));
	if (r.is_error)
		co_return text_result("Error: " + r.error, true);
	co_return text_result("message " + r.stop_reason + " to " + id);
}

araya::task<tool_result> interrupt_handler(std::shared_ptr<subagents_service> subs, tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto id = args ? araya::util::json::get_string(*args, "id") : std::string{};
	if (id.empty())
		co_return text_result("Error: interrupt_agent requires 'id'", true);
	if (!subs->interrupt(id))
		co_return text_result("no running turn for " + id);
	co_return text_result("interrupted " + id);
}

char const* status_name(child_status status) {
	switch (status) {
	case child_status::running:
		return "running";
	case child_status::idle:
		return "idle";
	case child_status::ready:
		return "ready";
	}
	return "ready";
}

araya::task<tool_result> list_handler(std::shared_ptr<subagents_service> subs, tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto scope = args ? araya::util::json::opt_string(*args, "scope") : std::nullopt;
	bool descendants = false;
	if (scope && !scope->empty()) {
		if (*scope == "children")
			descendants = false;
		else if (*scope == "descendants")
			descendants = true;
		else
			co_return text_result("Error: scope must be 'children' or 'descendants'", true);
	}
	if (ctx.session.empty())
		co_return text_result("Error: list_agents requires a calling session", true);

	auto const children = descendants ? subs->list_descendants(ctx.session) : subs->list_children(ctx.session);
	if (children.empty())
		co_return text_result("(no subagents)");
	std::string text;
	for (auto const& child : children) {
		if (!text.empty())
			text += "\n";
		text += child.id;
		text += " [";
		text += status_name(child.status);
		text += "]";
		if (descendants) {
			text += " parent=" + (child.parent.empty() ? std::string{"?"} : child.parent);
			text += " depth=" + std::to_string(child.depth);
		}
		if (!child.label.empty())
			text += " \u2014 " + child.label;
	}
	co_return text_result(std::move(text));
}

const tool_definition g_send_def{
	"send_message",
	"Send a message to a continuable child agent started by subagent. If the child is idle it starts "
	"a turn; if it is already working the message is delivered to it at its next step.",
	boost::json::value{
		{"type", "object"},
		{"properties",
		 boost::json::object{
			 {"id", boost::json::object{{"type", "string"}, {"description", "The child id returned by subagent."}}},
			 {"message", boost::json::object{{"type", "string"}, {"description", "The message to send."}}}}},
		{"required", boost::json::array{"id", "message"}}},
};

const tool_definition g_interrupt_def{
	"interrupt_agent",
	"Stop a running child agent's current turn. The child stays alive and keeps any queued messages.",
	boost::json::value{
		{"type", "object"},
		{"properties",
		 boost::json::object{
			 {"id", boost::json::object{{"type", "string"}, {"description", "The child id to interrupt."}}}}},
		{"required", boost::json::array{"id"}}},
};

const tool_definition g_list_def{
	"list_agents",
	"List your continuable background subagents by durable id and label. Use it to recall which ones you "
	"started, not to poll for completion \u2014 you are told when one finishes. `running` means the agent is "
	"working right now, `idle` means it is loaded but between turns, and `ready` means no live agent remains "
	"(resumable, not terminal); a send_message steers a running child at its nearest step boundary or starts "
	"a turn for an idle or ready child. Scope `descendants` walks the whole tree below you in stable "
	"pre-order, annotating each entry with its durable direct-parent id and depth.",
	boost::json::value{
		{"type", "object"},
		{"properties",
		 boost::json::object{
			 {"scope",
			  boost::json::object{
				  {"type", "string"},
				  {"enum", boost::json::array{"children", "descendants"}},
				  {"description",
				   "children (default) lists direct children only; descendants walks the complete tree below "
				   "you."}}}}}},
};

struct tool_subagent_control_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto tools = ctx.require<araya::tools::tools_service>(araya::tools::tools_key).shared();
		auto subs = ctx.require<subagents_service>(subagents_key).shared();
		tools->register_tool(
			ctx, g_send_def, [subs](tool_context const& call) { return send_message_handler(subs, call); });
		tools->register_tool(
			ctx, g_interrupt_def, [subs](tool_context const& call) { return interrupt_handler(subs, call); });
		tools->register_tool(ctx, g_list_def, [subs](tool_context const& call) { return list_handler(subs, call); });
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_tool_subagent_control(araya::plugin_config const&) {
	return std::make_unique<tool_subagent_control_plugin>();
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"subagents", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{
	"tool-subagent-control",
	g_deps,
	g_provs,
	&make_tool_subagent_control};

} // namespace
} // namespace araya::tool_subagent_control

namespace araya::tool_subagent_control {

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_subagent_control
