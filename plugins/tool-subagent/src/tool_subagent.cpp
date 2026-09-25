#include "araya/tool-subagent/tool_subagent.hpp"

#include "araya/config.hpp"
#include "araya/subagents/subagents.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace araya::tool_subagent {
namespace {

using namespace araya::subagents;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;

constexpr araya::config_key<std::string> provider_key{"provider"};
constexpr araya::config_key<std::string> tool_name_key{"tool_name"};
constexpr araya::config_key<std::string> background_mode_key{"background_mode"};
constexpr araya::config_key<std::uint32_t> max_depth_key{"max_depth"};
constexpr araya::config_key<std::string> child_provider_key{"child_provider"};
constexpr araya::config_key<std::string> child_model_key{"child_model"};
constexpr araya::config_key<std::string> reasoning_effort_key{"reasoning_effort"};

tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

struct subagent_config {
	std::string provider = "spawn";
	std::string tool_name = "subagent";
	bool continuable = true;
	std::uint32_t max_depth = 1;
	std::optional<std::string> child_provider;
	std::optional<std::string> child_model;
	std::optional<std::string> reasoning_effort;
};

subagent_config parse_config(araya::plugin_config const& config) {
	araya::plugin_config_view view(config);
	subagent_config out;
	if (auto value = view.try_get(provider_key))
		out.provider = *value;
	if (auto value = view.try_get(tool_name_key))
		out.tool_name = *value;
	if (auto value = view.try_get(background_mode_key)) {
		if (*value == "continuable")
			out.continuable = true;
		else if (*value == "one-shot" || *value == "one_shot" || *value == "oneshot")
			out.continuable = false;
		else
			throw araya::config_error("background_mode", *value, "expected continuable|one-shot");
	}
	if (auto value = view.try_get(max_depth_key))
		out.max_depth = *value;
	if (auto value = view.try_get(child_provider_key))
		out.child_provider = *value;
	if (auto value = view.try_get(child_model_key))
		out.child_model = *value;
	if (auto value = view.try_get(reasoning_effort_key))
		out.reasoning_effort = *value;
	return out;
}

araya::task<tool_result> subagent_handler(
	std::shared_ptr<subagents_service> subs,
	std::shared_ptr<subagent_config const> cfg,
	tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	if (!args)
		co_return text_result("Error: arguments must be a JSON object", true);
	auto prompt = araya::util::json::get_string(*args, "prompt");
	if (prompt.empty())
		co_return text_result("Error: subagent requires a non-empty 'prompt'", true);
	if (ctx.session.empty())
		co_return text_result("Error: subagent requires a calling session", true);

	start_request req;
	req.prompt = std::move(prompt);
	req.parent = ctx.session;
	req.label = araya::util::json::get_string(*args, "description");
	req.stop = ctx.stop;
	req.provider = cfg->child_provider;
	req.model = cfg->child_model;
	req.reasoning_effort = cfg->reasoning_effort;
	req.max_depth = cfg->max_depth;

	if (cfg->continuable) {
		auto r = co_await subs->start_continuable(cfg->provider, std::move(req));
		if (r.is_error)
			co_return text_result("Error: " + r.error, true);
		co_return text_result(
			"subagent started: " + r.child + " (running; its result arrives as a message when it settles)");
	}

	auto r = co_await subs->run(cfg->provider, std::move(req));
	if (r.is_error)
		co_return text_result("Error: " + r.error, true);
	std::string text = "subagent " + r.child + " " + r.stop_reason;
	if (!r.output.empty())
		text += "\n" + r.output;
	co_return text_result(std::move(text));
}

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

araya::task<tool_result> list_handler(std::shared_ptr<subagents_service> subs, tool_context const&) {
	auto children = subs->list_children();
	if (children.empty())
		co_return text_result("no child agents");
	std::string text;
	for (auto const& child : children) {
		if (!text.empty())
			text += "\n";
		text += child.id;
		if (!child.label.empty())
			text += " (" + child.label + ")";
		text += child.running ? " running" : " idle";
		if (child.pending > 0)
			text += ", " + std::to_string(child.pending) + " queued";
		if (!child.stop_reason.empty())
			text += ", last " + child.stop_reason;
	}
	co_return text_result(std::move(text));
}

const tool_definition g_subagent_def{
	"subagent",
	"Delegate a task to a child agent. In continuable mode the child is durable: it runs in the "
	"background and its result is delivered to you as a message when it settles; use send_message to "
	"continue it, interrupt_agent to stop its current turn, and list_agents to see its status.",
	boost::json::value{
		{"type", "object"},
		{"properties",
		 boost::json::object{
			 {"description",
			  boost::json::object{{"type", "string"}, {"description", "A short (3-5 word) description of the task."}}},
			 {"prompt",
			  boost::json::object{{"type", "string"}, {"description", "The task for the child to perform."}}}}},
		{"required", boost::json::array{"description", "prompt"}}},
};

const tool_definition g_send_def{
	"send_message",
	"Send a message to a continuable child agent started by subagent. If the child is idle it starts "
	"a turn; if it is already working the message is queued for its next turn boundary.",
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
	"List the child agents this session started, with whether each is running and how many messages are queued.",
	boost::json::value{{"type", "object"}, {"properties", boost::json::object{}}},
};

struct tool_subagent_plugin : araya::plugin {
	explicit tool_subagent_plugin(araya::plugin_config config)
		: config_(parse_config(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto tools = ctx.require<araya::tools::tools_service>(araya::tools::tools_key).shared();
		auto subs = ctx.require<subagents_service>(subagents_key).shared();
		auto cfg = std::make_shared<subagent_config const>(config_);

		tools->register_tool(
			ctx,
			tool_definition{config_.tool_name, g_subagent_def.description, g_subagent_def.parameters},
			[subs, cfg](tool_context const& call) { return subagent_handler(subs, cfg, call); });

		tools->register_tool(
			ctx, g_send_def, [subs](tool_context const& call) { return send_message_handler(subs, call); });
		tools->register_tool(
			ctx, g_interrupt_def, [subs](tool_context const& call) { return interrupt_handler(subs, call); });
		tools->register_tool(ctx, g_list_def, [subs](tool_context const& call) { return list_handler(subs, call); });
		co_return;
	}

private:
	subagent_config config_;
};

std::unique_ptr<araya::plugin> make_tool_subagent(araya::plugin_config const& config) {
	return std::make_unique<tool_subagent_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"tools", 1}, true, {}},
	{araya::service_id{"subagents", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"tool-subagent", g_deps, g_provs, &make_tool_subagent};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_subagent
