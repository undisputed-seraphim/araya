#include "araya/goal-round-driver/goal_round_driver.hpp"

#include "araya/agent-loop/agent.hpp"
#include "araya/agent-loop/events.hpp"
#include "araya/agent-loop/inbox.hpp"
#include "araya/goal/goal.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/session/events.hpp"
#include "araya/util/json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace araya::goal_round_driver {
namespace {

using araya::agent::agent_service;
using araya::agent::agent_status;
using araya::goal::goal_activation;
using araya::goal::goal_phase;
using araya::goal::goal_ref;
using araya::goal::goal_service;
using araya::goal::goal_view;
using araya::llm_bridge::message_source;
using araya::llm_bridge::user_message_data;
using araya::session::session_id;

// Render the complete goal-round instruction retained in session history (the
// harness's one-block prompt).
std::string render_goal_round_prompt(goal_view const& goal, std::uint64_t round) {
	return "<goal_round>\n"
		   "Objective: " +
		   boost::json::serialize(boost::json::value(goal.objective)) + "\nRound: " + std::to_string(round) + "/" +
		   std::to_string(goal.max_goal_rounds) +
		   "\n\nContinue working toward the objective in this same session. Treat the current workspace, tool "
		   "results, and durable session state as authoritative; inspect them instead of assuming earlier narration "
		   "is still current. Make concrete progress and verify the result. Before claiming completion, gather "
		   "evidence that the whole objective is achieved, read the current goal, and mark it complete. If work "
		   "remains, leave the goal active for the next round. Follow the configured goal-tool policy before "
		   "reporting a blocker.\n</goal_round>";
}

// The goal message source of a claimed round message, when present.
boost::json::object const* goal_source_of(boost::json::value const& message) {
	auto const* object = message.if_object();
	auto const* source = object ? object->if_contains("source") : nullptr;
	auto const* source_object = source ? source->if_object() : nullptr;
	if (!source_object)
		return nullptr;
	auto it = source_object->find("kind");
	if (it == source_object->end() || !it->value().is_string() || it->value().as_string() != "goal")
		return nullptr;
	return source_object;
}

// One session's scheduling state.
struct session_state {
	bool requested = false;
	bool running = false;
	bool competing = false;
};

// The live driver: serializes one round-scheduling pass per session and fans
// triggers onto it. Held in a shared_ptr so spawned tasks outlive a listener
// removal during teardown.
struct driver : std::enable_shared_from_this<driver> {
	boost::asio::any_io_executor executor;
	std::shared_ptr<goal_service> goals;
	std::shared_ptr<agent_service> agent;
	std::map<std::string, std::shared_ptr<session_state>> states;
	bool stopping = false;

	std::shared_ptr<session_state> state_of(std::string const& session) {
		auto& slot = states[session];
		if (!slot)
			slot = std::make_shared<session_state>();
		return slot;
	}

	void request_drive(std::string session) {
		if (stopping)
			return;
		auto state = state_of(session);
		state->requested = true;
		if (state->running)
			return;
		state->running = true;
		auto self = shared_from_this();
		boost::asio::co_spawn(
			executor,
			[self, session = std::move(session)]() -> araya::task<void> { co_await self->loop(std::move(session)); },
			boost::asio::detached);
	}

	araya::task<void> loop(std::string session) {
		for (;;) {
			auto state = state_of(session);
			if (!state->requested || stopping)
				break;
			state->requested = false;
			co_await drive(session);
		}
		state_of(session)->running = false;
	}

	araya::task<void> drive(std::string const& session) {
		auto state = state_of(session);
		if (stopping || state->competing)
			co_return;
		if (agent->status(session_id{session}) != agent_status::idle)
			co_return;
		std::optional<goal_view> goal;
		try {
			goal = goals->get(session_id{session});
		} catch (...) {
			co_return;
		}
		if (!goal || goal->phase != goal_phase::active || goal->activation != goal_activation::armed)
			co_return;
		if (goal->rounds_started >= goal->max_goal_rounds) {
			try {
				(void)goals->block(
					session_id{session},
					goal_ref{goal->id, goal->revision},
					araya::goal::goal_block_reason{
						"round-limit",
						"Goal reached its configured limit of " + std::to_string(goal->max_goal_rounds) + " rounds."});
			} catch (...) {
			}
			co_return;
		}
		auto const round = goal->rounds_started + 1;
		auto message = user_message_data(
			"goal-round-" + std::to_string(round),
			render_goal_round_prompt(*goal, round),
			message_source("goal", {{"goalId", goal->id}, {"revision", goal->revision}, {"round", round}}));
		try {
			co_await agent->followup(session_id{session}, std::move(message), {});
		} catch (...) {
			try {
				(void)goals->disarm(session_id{session});
			} catch (...) {
			}
		}
	}
};

struct goal_round_driver_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto goals = ctx.require<goal_service>(araya::goal::goals_key).shared();
		auto agent = ctx.require<agent_service>(araya::agent::agent_key).shared();

		auto d = std::make_shared<driver>();
		d->executor = ctx.executor();
		d->goals = std::move(goals);
		d->agent = std::move(agent);

		// A goal mutation (create/resume/complete/block/pause/clear) may arm,
		// change, or end auto-continuation: re-evaluate.
		ctx.on(araya::goal::goal_changed_key, [d](araya::goal::goal_changed_msg const& message) {
			d->request_drive(message.session.value);
		});

		// A human or producer wake clears its competing flag only once the
		// agent is idle again, then auto-continuation resumes.
		ctx.on(araya::agent::agent_status_key, [d](araya::agent::agent_status_msg const& message) {
			if (message.status != agent_status::idle)
				return;
			auto it = d->states.find(message.session.value);
			if (it != d->states.end())
				it->second->competing = false;
			d->request_drive(message.session.value);
		});

		// Non-goal next-turn input competes with the next automatic round.
		ctx.on(araya::agent::agent_inbox_inserted_key, [d](araya::agent::agent_inbox_inserted_msg const& message) {
			if (goal_source_of(message.message))
				return;
			auto inbox = d->agent->inbox(message.session);
			if (!inbox)
				return;
			auto id = araya::agent::inbox_message_id(message.message);
			bool next_turn = false;
			for (auto const& queued : inbox->next_turn)
				if (araya::agent::inbox_message_id(queued) == id)
					next_turn = true;
			if (next_turn)
				d->state_of(message.session.value)->competing = true;
		});

		// An errored or truncated turn drops automatic authority: never spin
		// rounds through failing turns.
		ctx.on(araya::session::appended_key, [d](araya::session::session_appended_msg const& message) {
			if (message.event.type != "turn/end")
				return;
			auto const* object = message.event.data.if_object();
			auto reason = object ? araya::util::json::get_string(*object, "reason") : std::string{};
			if (reason != "error" && reason != "max_tokens")
				return;
			try {
				(void)d->goals->disarm(message.id);
			} catch (...) {
			}
		});

		// Fence the reservation at the pre-step: drop a round that no longer
		// matches the exact live revision.
		ctx.on(
			araya::agent::pre_step_key,
			[d](araya::agent::pre_step_msg msg, araya::waterfall_continuation<araya::agent::pre_step_msg> next)
				-> araya::task<araya::agent::pre_step_msg> {
				bool saw_goal = false;
				bool valid = true;
				for (auto const& candidate : msg.messages) {
					auto const* source = goal_source_of(candidate);
					if (!source)
						continue;
					saw_goal = true;
					std::optional<goal_view> goal;
					try {
						goal = d->goals->get(msg.session);
					} catch (...) {
						valid = false;
						break;
					}
					if (!goal || goal->phase != goal_phase::active || goal->activation != goal_activation::armed ||
						araya::util::json::get_string(*source, "goalId") != goal->id ||
						araya::util::json::get_uint(*source, "revision") != goal->revision ||
						araya::util::json::get_uint(*source, "round") != goal->rounds_started + 1) {
						valid = false;
						break;
					}
				}
				if (!saw_goal)
					co_return co_await next(std::move(msg));
				if (!valid) {
					msg.reject = true;
					co_return msg;
				}
				co_return co_await next(std::move(msg));
			});

		// Teardown: stop scheduling and drop process-local authority.
		ctx.effect([d]() -> araya::cleanup_action {
			return [d] {
				d->stopping = true;
				for (auto const& [session, state] : d->states) {
					(void)state;
					try {
						(void)d->goals->disarm(session_id{session});
					} catch (...) {
					}
				}
			};
		});
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_goal_round_driver(araya::plugin_config const&) {
	return std::make_unique<goal_round_driver_plugin>();
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"goals", 1}, true, {}},
	{araya::service_id{"agent", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"goal-round-driver", g_deps, g_provs, &make_goal_round_driver};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::goal_round_driver
