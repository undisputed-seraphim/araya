#include "araya/tool-goal/tool_goal.hpp"

#include "araya/agent-loop/agent.hpp"
#include "araya/config.hpp"
#include "araya/goal/goal.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace araya::tool_goal {
namespace {

using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;

using araya::goal::goal_ref;
using araya::goal::goal_service;
using araya::goal::goal_view;
using araya::goal::goals_key;

constexpr araya::config_key<std::uint64_t> blocked_after_key{"blockedAfterConsecutiveRounds"};

// The authenticated call context: the exact calling session and the start
// sequence of the open model turn (the authority window).
struct execution {
	araya::session::session_id session;
	std::shared_ptr<araya::session::session> session_ptr;
	std::uint64_t open_turn_start_seq = 0;
};

tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

std::string_view phase_name(araya::goal::goal_phase phase) { return araya::goal::goal_phase_name(phase); }

// Validate and detach the cmp tool's goal value.
boost::json::value goal_value(std::optional<goal_view> const& goal) {
	if (!goal)
		return boost::json::object{{"goal", nullptr}};
	boost::json::object value;
	value["id"] = goal->id;
	value["revision"] = goal->revision;
	value["objective"] = goal->objective;
	value["phase"] = std::string(phase_name(goal->phase));
	value["roundsStarted"] = goal->rounds_started;
	value["maxGoalRounds"] = goal->max_goal_rounds;
	if (goal->blocked_reason) {
		value["blockedReason"] =
			boost::json::object{{"code", goal->blocked_reason->code}, {"message", goal->blocked_reason->message}};
	}
	return boost::json::object{
		{"goal", std::move(value)}, {"activation", std::string(araya::goal::goal_activation_name(goal->activation))}};
}

// Resolve and authenticate the calling session and its open turn.
execution require_execution(araya::agent::agent_service& agent, session_store& store, tool_context const& ctx) {
	if (ctx.session.empty())
		throw std::runtime_error("goal tools require a calling agent session");
	auto session = store.get(session_id{ctx.session});
	if (!session)
		throw std::runtime_error("goal tools: the calling session is not entered");
	auto const* boundary = agent.turn_boundary(session->id());
	if (!boundary || !boundary->open_turn_start_seq)
		throw std::runtime_error("goal tools require an open model turn");
	return execution{session->id(), session, *boundary->open_turn_start_seq};
}

template <class Pred>
bool some_open_turn_event(execution const& exec, Pred predicate) {
	auto const& log = exec.session_ptr->log();
	for (std::uint64_t seq = exec.open_turn_start_seq + 1; seq < log.size(); ++seq) {
		if (predicate(log[seq]))
			return true;
	}
	return false;
}

std::string_view message_source_kind(araya::session::session_event const& event) {
	if (event.type != "user/message")
		return {};
	auto const* object = event.data.if_object();
	auto const* source = object ? object->if_contains("source") : nullptr;
	auto const* source_object = source ? source->if_object() : nullptr;
	if (!source_object)
		return {};
	auto it = source_object->find("kind");
	if (it == source_object->end() || !it->value().is_string())
		return {};
	return it->value().as_string();
}

boost::json::object const* message_source(araya::session::session_event const& event) {
	if (event.type != "user/message")
		return nullptr;
	auto const* object = event.data.if_object();
	auto const* source = object ? object->if_contains("source") : nullptr;
	return source ? source->if_object() : nullptr;
}

// Host-attested human input in the current root-session turn.
bool has_direct_human(execution const& exec) {
	if (exec.session_ptr->header().delegation_depth != 0)
		return false;
	return some_open_turn_event(
		exec, [](araya::session::session_event const& event) { return message_source_kind(event) == "user"; });
}

// This turn is the current goal's exact admitted round.
bool is_matching_goal_round(execution const& exec, goal_view const& goal) {
	return some_open_turn_event(exec, [&](araya::session::session_event const& event) {
		auto const* source = message_source(event);
		if (!source || araya::util::json::get_string(*source, "kind") != "goal")
			return false;
		return araya::util::json::get_string(*source, "goalId") == goal.id &&
			   araya::util::json::get_uint(*source, "revision") == goal.revision &&
			   araya::util::json::get_uint(*source, "round") == goal.rounds_started;
	});
}

void require_direct_human(execution const& exec) {
	if (!has_direct_human(exec))
		throw std::runtime_error("this goal operation requires a direct human turn on a top-level agent");
}

// Direct-human or the exact goal round; otherwise reject.
bool completion_authority(execution const& exec, goal_view const& goal) {
	if (has_direct_human(exec))
		return true;
	if (is_matching_goal_round(exec, goal))
		return true;
	throw std::runtime_error("complete and blocked require a direct human turn or the current goal round");
}

std::optional<std::string> optional_text(boost::json::object const& object, std::string_view key) {
	auto it = object.find(key);
	if (it == object.end() || !it->value().is_string())
		return std::nullopt;
	std::string text(it->value().as_string());
	if (text.empty())
		return std::nullopt;
	return text;
}

std::optional<std::uint64_t> optional_rounds(boost::json::object const& object, std::string_view key) {
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	auto const* value = it->value().if_int64();
	std::int64_t number =
		value ? *value : (it->value().is_uint64() ? static_cast<std::int64_t>(it->value().as_uint64()) : 0);
	if (number <= 0)
		return std::nullopt;
	return static_cast<std::uint64_t>(number);
}

std::string guidance(std::uint64_t blocked_after) {
	return "Use goal tools for one long-running completion objective in the current session. create_goal may infer "
		   "goal intent from a direct human request in any language; do not create a goal for routine single-turn "
		   "work. Call get_goal before update_goal and copy its exact goal_id and revision. After session resume or "
		   "fork, an active goal is disarmed: when a human asks to continue or resume in any wording or language, "
		   "use update_goal action resume to rearm it. Mark complete only when the objective is actually achieved. "
		   "Mark blocked only after the same blocking condition persists for at least " +
		   std::to_string(blocked_after) +
		   " consecutive goal rounds, and report that concrete condition in blocked_reason; difficulty, uncertainty, "
		   "or useful remaining work is not blocked.";
}

boost::json::value goal_output_schema() { return boost::json::value{{"type", "object"}}; }

constexpr std::string_view k_create_description =
	"Create one persisted same-session completion goal when the current direct human request is a long-running "
	"objective that should continue across autonomous goal rounds. You may infer that intent without requiring the "
	"user to say \"create a goal\". Do not use this for trivial single-turn work. Execution rejects non-human and "
	"subagent authority.";
constexpr std::string_view k_get_description =
	"Read the current same-session goal, including its exact id/revision, objective, phase, completed continuation "
	"rounds, round limit, blocker reason when present, and whether another continuation is armed. Call this before "
	"updating a goal.";
constexpr std::string_view k_update_description =
	"Update the exact current goal revision. edit, pause, and resume require a direct top-level human request. "
	"During an automatic continuation of the current goal, complete and blocked are also allowed. blocked is "
	"rejected before the configured minimum round count; the model remains responsible for judging that the same "
	"condition persisted across those rounds and must explain it in blocked_reason.";

struct tool_goal_plugin : araya::plugin {
	explicit tool_goal_plugin(araya::plugin_config const& config) {
		araya::plugin_config_view const view(config);
		if (auto value = view.try_get(blocked_after_key))
			blocked_after_ = *value;
	}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto goals = ctx.require<goal_service>(goals_key).shared();
		auto tools = ctx.require<tools_service>(tools_key).shared();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		auto store = ctx.require<session_store>(sessions_key).shared();
		auto agent = ctx.require<araya::agent::agent_service>(araya::agent::agent_key).shared();

		araya::system_prompt::prompt_section section;
		section.name = "tool:goal";
		section.order = araya::system_prompt::section_order("TOOL_GOAL");
		section.text = guidance(blocked_after_);
		prompts->section(ctx, std::move(section));

		auto blocked_after = blocked_after_;
		auto get_handler = [goals, store, agent](tool_context const& call) -> araya::task<tool_result> {
			auto exec = require_execution(*agent, *store, call);
			co_return text_result(boost::json::serialize(goal_value(goals->get(exec.session))));
		};
		tools->register_tool(
			ctx,
			tool_definition{"get_goal", std::string(k_get_description), goal_output_schema()},
			std::move(get_handler));

		auto create_handler = [goals, store, agent](tool_context const& call) -> araya::task<tool_result> {
			auto exec = require_execution(*agent, *store, call);
			require_direct_human(exec);
			auto const* object = call.arguments.if_object();
			if (!object)
				throw std::runtime_error("create_goal requires an arguments object");
			auto objective = araya::util::json::get_string(*object, "objective");
			auto cap = optional_rounds(*object, "max_goal_rounds");
			auto goal = goals->create(exec.session, objective, cap);
			co_return text_result(boost::json::serialize(goal_value(goal)));
		};
		tools->register_tool(
			ctx,
			tool_definition{
				"create_goal",
				std::string(k_create_description),
				boost::json::value{
					{"type", "object"},
					{"properties",
					 boost::json::object{
						 {"objective", boost::json::object{{"type", "string"}, {"required", true}}},
						 {"max_goal_rounds", boost::json::object{{"type", "number"}}}}},
					{"required", boost::json::array{"objective"}}}},
			std::move(create_handler));

		auto update_handler =
			[goals, store, agent, blocked_after](tool_context const& call) -> araya::task<tool_result> {
			auto exec = require_execution(*agent, *store, call);
			auto const* object = call.arguments.if_object();
			if (!object)
				throw std::runtime_error("update_goal requires an arguments object");
			goal_ref ref{
				araya::util::json::get_string(*object, "goal_id"), araya::util::json::get_uint(*object, "revision")};
			auto action = araya::util::json::get_string(*object, "action");
			auto objective = optional_text(*object, "objective");
			auto cap = optional_rounds(*object, "max_goal_rounds");
			auto blocked_reason = optional_text(*object, "blocked_reason");

			if (action == "edit") {
				require_direct_human(exec);
				if (blocked_reason)
					throw std::runtime_error("blocked_reason is valid only with action blocked");
				auto goal = goals->edit(exec.session, ref, objective, cap);
				co_return text_result(boost::json::serialize(goal_value(goal)));
			}
			if (action == "pause" || action == "resume") {
				require_direct_human(exec);
				if (objective || cap || blocked_reason)
					throw std::runtime_error(
						"objective and max_goal_rounds are valid only with action edit; blocked_reason is valid only "
						"with action blocked");
				auto current = goals->get(exec.session);
				if (action == "resume" && current && current->id == ref.id && current->revision == ref.revision &&
					current->phase == araya::goal::goal_phase::paused)
					throw std::runtime_error("the model cannot resume a paused goal; the user must resume it");
				auto goal = action == "pause" ? goals->pause(exec.session, ref) : goals->resume(exec.session, ref);
				co_return text_result(boost::json::serialize(goal_value(goal)));
			}
			// complete | blocked
			auto current = goals->get(exec.session);
			if (!current)
				throw std::runtime_error("no current goal");
			completion_authority(exec, *current);
			if (objective || cap)
				throw std::runtime_error("objective and max_goal_rounds are valid only with action edit");
			if (action == "complete") {
				if (blocked_reason)
					throw std::runtime_error("blocked_reason is valid only with action blocked");
				auto goal = goals->complete(exec.session, ref);
				co_return text_result(boost::json::serialize(goal_value(goal)));
			}
			if (action == "blocked") {
				if (!blocked_reason)
					throw std::runtime_error("blocked_reason is required with action blocked");
				if (!has_direct_human(exec) && current->rounds_started < blocked_after)
					throw std::runtime_error(
						"blocked requires at least " + std::to_string(blocked_after) +
						" consecutive goal rounds; current round is " + std::to_string(current->rounds_started));
				auto goal =
					goals->block(exec.session, ref, araya::goal::goal_block_reason{"model-reported", *blocked_reason});
				co_return text_result(boost::json::serialize(goal_value(goal)));
			}
			throw std::runtime_error("update_goal: unknown action '" + action + "'");
		};
		tools->register_tool(
			ctx,
			tool_definition{
				"update_goal",
				std::string(k_update_description),
				boost::json::value{
					{"type", "object"},
					{"properties",
					 boost::json::object{
						 {"goal_id", boost::json::object{{"type", "string"}, {"required", true}}},
						 {"revision", boost::json::object{{"type", "number"}, {"required", true}}},
						 {"action",
						  boost::json::object{
							  {"type", "string"},
							  {"required", true},
							  {"enum", boost::json::array{"edit", "pause", "resume", "complete", "blocked"}}}},
						 {"objective", boost::json::object{{"type", "string"}}},
						 {"max_goal_rounds", boost::json::object{{"type", "number"}}},
						 {"blocked_reason", boost::json::object{{"type", "string"}}}}},
					{"required", boost::json::array{"goal_id", "revision", "action"}}}},
			std::move(update_handler));

		co_return;
	}

private:
	std::uint64_t blocked_after_ = 3;
};

std::unique_ptr<araya::plugin> make_tool_goal(araya::plugin_config const& config) {
	return std::make_unique<tool_goal_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"goals", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
	{araya::service_id{"agent", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"tool-goal", g_deps, g_provs, &make_tool_goal};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_goal
