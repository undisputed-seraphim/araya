#include "commands.hpp"

#include "araya/goal/goal.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <utility>

// The human goal surface: inspect and control the current session's goal.
// The local equivalent of the harness's `command-goal` (plugins cannot
// register slash commands here, so it lives in the app command table). It is
// also the only way to resume a paused goal: the model is forbidden from
// resuming a paused goal, mirroring the harness.
namespace araya::app {

namespace {

std::string goal_line(std::optional<araya::goal::goal_view> const& goal) {
	if (!goal)
		return "goal: none";
	std::string line = "goal: " + goal->id + " r" + std::to_string(goal->revision) + " " +
					   araya::goal::goal_phase_name(goal->phase) + " rounds " + std::to_string(goal->rounds_started) +
					   "/" + std::to_string(goal->max_goal_rounds) + " " +
					   araya::goal::goal_activation_name(goal->activation);
	if (goal->blocked_reason)
		line += " blocked:" + goal->blocked_reason->code;
	line += " \"" + goal->objective + "\"";
	return line;
}

} // namespace

araya::task<void> cmd_goal(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string command;
		is >> command;
		std::string sub;
		is >> sub;
		std::string rest;
		std::getline(is, rest);
		rest = trim(rest);

		if (!ctx.current) {
			out("goal: no current session");
			co_return;
		}
		auto root = ctx.rt->root_context();
		auto goals = root.require<araya::goal::goal_service>(araya::goal::goals_key).shared();
		auto const session = *ctx.current;

		if (sub.empty() || sub == "get" || sub == "status") {
			out(goal_line(goals->get(session)));
			co_return;
		}
		if (sub == "create") {
			if (rest.empty()) {
				out("goal: usage: goal create <objective>");
				co_return;
			}
			out(goal_line(goals->create(session, rest, std::nullopt)));
			co_return;
		}
		auto current = goals->get(session);
		if (!current) {
			out("goal: none to " + sub);
			co_return;
		}
		araya::goal::goal_ref ref{current->id, current->revision};
		if (sub == "pause") {
			out(goal_line(goals->pause(session, ref)));
			co_return;
		}
		if (sub == "resume") {
			out(goal_line(goals->resume(session, ref)));
			co_return;
		}
		if (sub == "complete") {
			out(goal_line(goals->complete(session, ref)));
			co_return;
		}
		if (sub == "disarm") {
			out(goal_line(goals->disarm(session)));
			co_return;
		}
		if (sub == "clear") {
			(void)goals->clear(session, ref);
			out("goal: cleared");
			co_return;
		}
		out("goal: unknown subcommand '" + sub + "'");
	} catch (std::exception const& e) {
		out(std::string("goal: ") + e.what());
	}
	co_return;
}

} // namespace araya::app
