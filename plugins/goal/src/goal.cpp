#include "araya/goal/goal.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::goal {

namespace {

// Indexed by goal_phase; keep in enum order.
constexpr char const* k_phase_names[]{"active", "paused", "blocked", "complete"};

std::optional<goal_phase> phase_of(std::string_view name) {
	for (std::size_t i = 0; i < std::size(k_phase_names); ++i) {
		if (name == k_phase_names[i])
			return static_cast<goal_phase>(i);
	}
	return std::nullopt;
}

std::string mint_goal_id() {
	auto hex16 = [](std::uint64_t value) {
		static constexpr char digits[] = "0123456789abcdef";
		std::string out(16, '0');
		for (int i = 15; i >= 0; --i) {
			out[static_cast<std::size_t>(i)] = digits[value & 0xF];
			value >>= 4;
		}
		return out;
	};
	std::random_device device;
	std::uint64_t random = (static_cast<std::uint64_t>(device()) << 32) ^ device();
	random ^= static_cast<std::uint64_t>(araya::session::now_ms()) << 16;
	return "goal-" + hex16(static_cast<std::uint64_t>(araya::session::now_ms())) + "-" + hex16(random);
}

std::string trim(std::string_view text) {
	auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
		return {};
	auto last = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(first, last - first + 1));
}

std::optional<std::uint64_t> as_uint(boost::json::value const& node) {
	if (node.is_uint64())
		return node.as_uint64();
	if (node.is_int64() && node.as_int64() >= 0)
		return static_cast<std::uint64_t>(node.as_int64());
	return std::nullopt;
}

std::optional<std::int64_t> as_int(boost::json::value const& node) {
	if (node.is_int64())
		return node.as_int64();
	if (node.is_uint64())
		return static_cast<std::int64_t>(node.as_uint64());
	return std::nullopt;
}

std::optional<std::string> string_field(boost::json::object const& object, std::string_view key) {
	auto it = object.find(key);
	if (it == object.end() || !it->value().is_string())
		return std::nullopt;
	return std::string(it->value().as_string());
}

// The operation an `goal/change` event records.
enum class operation_kind : std::uint8_t {
	create,
	edit,
	pause,
	resume,
	complete,
	block,
	clear,
};

std::optional<operation_kind> operation_of(std::string_view name) {
	if (name == "create")
		return operation_kind::create;
	if (name == "edit")
		return operation_kind::edit;
	if (name == "pause")
		return operation_kind::pause;
	if (name == "resume")
		return operation_kind::resume;
	if (name == "complete")
		return operation_kind::complete;
	if (name == "block")
		return operation_kind::block;
	if (name == "clear")
		return operation_kind::clear;
	return std::nullopt;
}

// A decoded, shape-validated goal change.
struct decoded_change {
	operation_kind operation = operation_kind::create;
	goal_snapshot goal;
	std::uint64_t rounds_started = 0;
	std::int64_t created_at = 0;
	std::int64_t updated_at = 0;
	goal_ref cleared;
	std::int64_t cleared_at = 0;
};

goal_snapshot decode_snapshot(boost::json::object const& object) {
	goal_snapshot goal;
	auto id = string_field(object, "id");
	auto objective = string_field(object, "objective");
	auto revision = object.if_contains("revision");
	auto max_rounds = object.if_contains("maxGoalRounds");
	auto phase_name = string_field(object, "phase");
	if (!id || id->empty())
		throw std::runtime_error("goal change goal.id must be a non-empty string");
	if (!objective || trim(*objective).empty() || *objective != trim(*objective))
		throw std::runtime_error("goal change goal.objective must be non-empty and normalized");
	if (!revision || !as_uint(*revision) || *as_uint(*revision) < 1)
		throw std::runtime_error("goal change goal.revision must be a positive integer");
	if (!max_rounds || !as_uint(*max_rounds) || *as_uint(*max_rounds) < 1)
		throw std::runtime_error("goal change goal.maxGoalRounds must be a positive integer");
	if (!phase_name)
		throw std::runtime_error("goal change goal.phase is invalid");
	auto phase = phase_of(*phase_name);
	if (!phase)
		throw std::runtime_error("goal change goal.phase is invalid");
	goal.id = *id;
	goal.objective = *objective;
	goal.revision = *as_uint(*revision);
	goal.max_goal_rounds = *as_uint(*max_rounds);
	goal.phase = *phase;
	if (*phase == goal_phase::blocked) {
		auto blocked = object.if_contains("blockedReason");
		auto const* reason = blocked ? blocked->if_object() : nullptr;
		if (!reason)
			throw std::runtime_error("goal change goal.blockedReason must be present while blocked");
		auto code = string_field(*reason, "code");
		auto message = string_field(*reason, "message");
		if (!code || code->empty() || !message || trim(*message).empty())
			throw std::runtime_error("goal change goal.blockedReason is invalid");
		goal.blocked_reason = goal_block_reason{*code, trim(*message)};
	}
	return goal;
}

decoded_change decode_change(boost::json::value const& data) {
	auto const* object = data.if_object();
	if (!object)
		throw std::runtime_error("goal change must be a JSON object");
	if (string_field(*object, "kind") != "goal/change")
		throw std::runtime_error("goal change has an invalid kind");
	auto version = object->if_contains("version");
	if (!version || !as_uint(*version) || *as_uint(*version) != 1)
		throw std::runtime_error("unsupported goal change version");
	auto operation_name_value = string_field(*object, "operation");
	if (!operation_name_value)
		throw std::runtime_error("goal change operation is invalid");
	auto operation = operation_of(*operation_name_value);
	if (!operation)
		throw std::runtime_error("goal change operation is invalid");

	decoded_change change;
	change.operation = *operation;
	if (*operation == operation_kind::clear) {
		auto cleared = object->if_contains("cleared");
		auto const* ref = cleared ? cleared->if_object() : nullptr;
		if (!ref)
			throw std::runtime_error("goal clear change lacks a cleared ref");
		auto id = string_field(*ref, "id");
		auto revision = ref->if_contains("revision");
		if (!id || id->empty() || !revision || !as_uint(*revision) || *as_uint(*revision) < 1)
			throw std::runtime_error("goal clear change ref is invalid");
		change.cleared = goal_ref{*id, *as_uint(*revision)};
		auto cleared_at = object->if_contains("clearedAt");
		if (!cleared_at || !as_int(*cleared_at) || *as_int(*cleared_at) < 0)
			throw std::runtime_error("goal clear change clearedAt is invalid");
		change.cleared_at = *as_int(*cleared_at);
		return change;
	}

	auto goal = object->if_contains("goal");
	if (!goal || !goal->is_object())
		throw std::runtime_error("goal change lacks a goal snapshot");
	change.goal = decode_snapshot(goal->as_object());
	auto rounds = object->if_contains("roundsStarted");
	auto created = object->if_contains("createdAt");
	auto updated = object->if_contains("updatedAt");
	if (!rounds || !as_uint(*rounds))
		throw std::runtime_error("goal change roundsStarted is invalid");
	if (!created || !as_int(*created) || *as_int(*created) < 0)
		throw std::runtime_error("goal change createdAt is invalid");
	if (!updated || !as_int(*updated) || *as_int(*updated) < 0)
		throw std::runtime_error("goal change updatedAt is invalid");
	if (*as_int(*updated) < *as_int(*created))
		throw std::runtime_error("goal change updatedAt cannot precede createdAt");
	change.rounds_started = *as_uint(*rounds);
	change.created_at = *as_int(*created);
	change.updated_at = *as_int(*updated);
	return change;
}

void require_same_definition(goal_snapshot const& current, goal_snapshot const& next) {
	if (next.objective != current.objective || next.max_goal_rounds != current.max_goal_rounds)
		throw std::runtime_error("goal operation cannot change objective or maxGoalRounds");
}

// Apply one decoded change to the projection state, throwing on invalid
// transitions (the projection captures the throw in `failure`).
void apply_change(goal_projection_state& state, decoded_change const& change) {
	if (change.operation == operation_kind::clear) {
		if (!state.current)
			throw std::runtime_error("goal clear requires a current goal");
		if (change.cleared.id != state.current->id || change.cleared.revision != state.current->revision + 1)
			throw std::runtime_error("goal clear must advance the current goal by one revision");
		if (change.cleared_at < state.updated_at)
			throw std::runtime_error("goal clear timestamp cannot precede the current goal update");
		state.current.reset();
		state.rounds_started = 0;
		state.created_at = 0;
		state.updated_at = 0;
		state.last_ref = change.cleared;
		return;
	}

	auto const& goal = change.goal;
	if (change.operation == operation_kind::create) {
		bool seen = std::find(state.seen_ids.begin(), state.seen_ids.end(), goal.id) != state.seen_ids.end();
		if (goal.revision != 1 || goal.phase != goal_phase::active || change.rounds_started != 0 || seen ||
			(state.current && state.current->phase != goal_phase::complete))
			throw std::runtime_error("goal create requires a fresh active revision-one goal with zero rounds");
		state.seen_ids.push_back(goal.id);
	} else {
		if (!state.current)
			throw std::runtime_error("goal operation requires a current goal");
		auto const& current = *state.current;
		if (goal.id != current.id || goal.revision != current.revision + 1)
			throw std::runtime_error("goal operation must advance the current goal by one revision");
		if (change.created_at != state.created_at || change.updated_at < state.updated_at ||
			change.rounds_started != state.rounds_started)
			throw std::runtime_error("goal operation does not preserve the current counters and timestamps");
		switch (change.operation) {
		case operation_kind::edit:
			if (goal.phase != current.phase)
				throw std::runtime_error("goal edit cannot change phase");
			break;
		case operation_kind::pause:
			require_same_definition(current, goal);
			if (current.phase != goal_phase::active || goal.phase != goal_phase::paused)
				throw std::runtime_error("goal pause has an invalid phase transition");
			break;
		case operation_kind::resume:
			require_same_definition(current, goal);
			if ((current.phase != goal_phase::active && current.phase != goal_phase::paused &&
				 current.phase != goal_phase::blocked) ||
				goal.phase != goal_phase::active || state.rounds_started >= goal.max_goal_rounds)
				throw std::runtime_error("goal resume has an invalid phase transition or exhausted budget");
			break;
		case operation_kind::complete:
			require_same_definition(current, goal);
			if (current.phase == goal_phase::complete || goal.phase != goal_phase::complete)
				throw std::runtime_error("goal complete has an invalid phase transition");
			break;
		case operation_kind::block:
			require_same_definition(current, goal);
			if (current.phase != goal_phase::active || goal.phase != goal_phase::blocked)
				throw std::runtime_error("goal block has an invalid phase transition");
			break;
		default:
			throw std::runtime_error("unknown goal snapshot operation");
		}
	}
	state.current = goal;
	state.rounds_started = change.rounds_started;
	state.created_at = change.created_at;
	state.updated_at = change.updated_at;
	state.last_ref = goal_ref{goal.id, goal.revision};
}

// Apply one goal-sourced user message (round admission).
void apply_round(goal_projection_state& state, boost::json::object const& source) {
	auto goal_id = string_field(source, "goalId");
	auto revision = source.if_contains("revision");
	auto round = source.if_contains("round");
	if (!goal_id || goal_id->empty() || !revision || !as_uint(*revision) || !round || !as_uint(*round) ||
		*as_uint(*round) < 1)
		throw std::runtime_error("goal message source is invalid");
	if (!state.current || state.current->phase != goal_phase::active || *goal_id != state.current->id ||
		*as_uint(*revision) != state.current->revision || *as_uint(*round) != state.rounds_started + 1 ||
		*as_uint(*round) > state.current->max_goal_rounds)
		throw std::runtime_error("goal round is not the next admitted round of the active goal");
	state.rounds_started = *as_uint(*round);
}

boost::json::value snapshot_json(goal_snapshot const& goal) {
	boost::json::object object;
	object["id"] = goal.id;
	object["revision"] = goal.revision;
	object["objective"] = goal.objective;
	object["phase"] = std::string(goal_phase_name(goal.phase));
	object["maxGoalRounds"] = goal.max_goal_rounds;
	if (goal.blocked_reason) {
		object["blockedReason"] =
			boost::json::object{{"code", goal.blocked_reason->code}, {"message", goal.blocked_reason->message}};
	}
	return object;
}

} // namespace

char const* goal_phase_name(goal_phase phase) noexcept {
	auto index = static_cast<std::size_t>(phase);
	return index < std::size(k_phase_names) ? k_phase_names[index] : "active";
}

char const* goal_activation_name(goal_activation activation) noexcept {
	return activation == goal_activation::armed ? "armed" : "disarmed";
}

char const* goal_error_name(goal_error_code code) noexcept {
	switch (code) {
	case goal_error_code::agent_not_live:
		return "GOAL_AGENT_NOT_LIVE";
	case goal_error_code::not_found:
		return "GOAL_NOT_FOUND";
	case goal_error_code::already_exists:
		return "GOAL_ALREADY_EXISTS";
	case goal_error_code::stale_revision:
		return "GOAL_STALE_REVISION";
	case goal_error_code::invalid_objective:
		return "GOAL_INVALID_OBJECTIVE";
	case goal_error_code::invalid_max_rounds:
		return "GOAL_INVALID_MAX_ROUNDS";
	case goal_error_code::invalid_block_reason:
		return "GOAL_INVALID_BLOCK_REASON";
	case goal_error_code::invalid_edit:
		return "GOAL_INVALID_EDIT";
	case goal_error_code::invalid_transition:
		return "GOAL_INVALID_TRANSITION";
	}
	return "GOAL_INVALID_TRANSITION";
}

goal_error::goal_error(std::string message, goal_error_code code)
	: std::runtime_error(std::move(message))
	, code(code) {}

araya::session::event_projection<goal_projection_state> goal_projection() {
	return araya::session::event_projection<goal_projection_state>{
		.name = "goal",
		.types = {std::string(k_goal_change_event), "user/message"},
		.init = [](araya::session::session_header const&) { return goal_projection_state{}; },
		.apply =
			[](goal_projection_state& state, araya::session::session_event const& event) {
				if (state.failure)
					return;
				if (event.type == "user/message") {
					auto const* object = event.data.if_object();
					if (!object)
						return;
					auto source = object->if_contains("source");
					auto const* source_object = source ? source->if_object() : nullptr;
					if (!source_object || string_field(*source_object, "kind") != "goal")
						return;
					try {
						apply_round(state, *source_object);
					} catch (std::exception const& e) {
						state.failure = std::string("goal replay failed at session event ") +
										std::to_string(event.seq) + ": " + e.what();
					}
					return;
				}
				try {
					apply_change(state, decode_change(event.data));
				} catch (std::exception const& e) {
					state.failure = std::string("goal replay failed at session event ") + std::to_string(event.seq) +
									": " + e.what();
				}
			}};
}

// -- service ---------------------------------------------------------------

goal_service::goal_service(
	std::shared_ptr<araya::event_bus> bus,
	std::shared_ptr<araya::session::session_store> store,
	araya::session::projection_state<goal_projection_state> projection,
	std::uint64_t default_max_goal_rounds)
	: bus_(std::move(bus))
	, store_(std::move(store))
	, projection_(std::move(projection))
	, default_max_goal_rounds_(default_max_goal_rounds) {}

goal_view goal_service::ensure_session(araya::session::session_id const& session) const {
	if (!store_->get(session))
		throw goal_error("agent is not a live session '" + session.value + "'", goal_error_code::agent_not_live);
	return {};
}

goal_projection_state const& goal_service::expect_state(araya::session::session_id const& session) const {
	auto state = projection_.state_of(session);
	if (!state)
		throw goal_error("session '" + session.value + "' has no goal projection", goal_error_code::agent_not_live);
	if (state->failure)
		throw goal_error(*state->failure, goal_error_code::invalid_transition);
	return *state;
}

void goal_service::expect_current(goal_projection_state const& state, goal_ref const& ref) const {
	if (!state.current)
		throw goal_error("no current goal", goal_error_code::not_found);
	if (ref.id != state.current->id || ref.revision != state.current->revision)
		throw goal_error(
			"stale goal ref '" + ref.id + "' revision " + std::to_string(ref.revision),
			goal_error_code::stale_revision);
}

goal_activation goal_service::activation_of(araya::session::session_id const& session) const {
	auto it = activations_.find(session);
	return it == activations_.end() ? goal_activation::disarmed : it->second;
}

void goal_service::set_activation(araya::session::session_id const& session, goal_activation activation) {
	activations_.insert_or_assign(session, activation);
}

goal_activation goal_service::activation(araya::session::session_id const& session) const {
	return activation_of(session);
}

void goal_service::forget(araya::session::session_id const& session) { activations_.erase(session); }

goal_view goal_service::view_of(goal_projection_state const& state, goal_activation activation) const {
	goal_view view;
	if (state.current) {
		view.id = state.current->id;
		view.revision = state.current->revision;
		view.objective = state.current->objective;
		view.phase = state.current->phase;
		view.blocked_reason = state.current->blocked_reason;
		view.max_goal_rounds = state.current->max_goal_rounds;
	}
	view.rounds_started = state.rounds_started;
	view.created_at = state.created_at;
	view.updated_at = state.updated_at;
	view.activation = activation;
	return view;
}

void goal_service::commit_change(
	araya::session::session_id const& session,
	boost::json::value const& change,
	goal_activation activation) {
	auto session_ptr = store_->get(session);
	if (!session_ptr)
		throw goal_error("agent is not a live session '" + session.value + "'", goal_error_code::agent_not_live);
	set_activation(session, activation);
	session_ptr->append(std::string(k_goal_change_event), change);
	auto const& state = expect_state(session);
	goal_changed_msg message;
	message.session = session;
	message.operation = string_field(change.as_object(), "operation").value_or("clear");
	if (state.current) {
		message.ref = goal_ref{state.current->id, state.current->revision};
		message.goal = view_of(state, activation);
	} else if (state.last_ref) {
		message.ref = *state.last_ref;
	}
	if (bus_)
		bus_->dispatch(goal_changed_key, std::move(message));
}

goal_view goal_service::commit_snapshot(
	araya::session::session_id const& session,
	std::string_view operation,
	goal_snapshot const& goal,
	std::uint64_t rounds_started,
	std::int64_t created_at,
	std::int64_t updated_at,
	goal_activation activation) {
	boost::json::object change;
	change["kind"] = "goal/change";
	change["version"] = 1;
	change["operation"] = std::string(operation);
	change["goal"] = snapshot_json(goal);
	change["roundsStarted"] = rounds_started;
	change["createdAt"] = created_at;
	change["updatedAt"] = updated_at;
	commit_change(session, std::move(change), activation);
	auto const& state = expect_state(session);
	return view_of(state, activation);
}

goal_view goal_service::commit_current(
	araya::session::session_id const& session,
	std::string_view operation,
	goal_snapshot const& goal,
	goal_activation activation) {
	auto const& state = expect_state(session);
	return commit_snapshot(
		session,
		operation,
		goal,
		state.rounds_started,
		state.created_at,
		std::max(araya::session::now_ms(), state.updated_at),
		activation);
}

goal_view goal_service::create(
	araya::session::session_id const& session,
	std::string objective,
	std::optional<std::uint64_t> max_rounds) {
	ensure_session(session);
	auto normalized = trim(objective);
	if (normalized.empty())
		throw goal_error("goal objective must be a non-empty string", goal_error_code::invalid_objective);
	std::uint64_t cap = max_rounds.value_or(default_max_goal_rounds_);
	if (cap < 1)
		throw goal_error("maxGoalRounds must be a positive integer", goal_error_code::invalid_max_rounds);
	auto const& state = expect_state(session);
	if (state.current && state.current->phase != goal_phase::complete)
		throw goal_error("goal '" + state.current->id + "' already exists", goal_error_code::already_exists);
	goal_snapshot goal;
	goal.id = mint_goal_id();
	goal.revision = 1;
	goal.objective = normalized;
	goal.phase = goal_phase::active;
	goal.max_goal_rounds = cap;
	auto now = araya::session::now_ms();
	return commit_snapshot(session, "create", goal, 0, now, now, goal_activation::armed);
}

goal_view goal_service::edit(
	araya::session::session_id const& session,
	goal_ref const& ref,
	std::optional<std::string> objective,
	std::optional<std::uint64_t> max_rounds) {
	ensure_session(session);
	auto const& state = expect_state(session);
	expect_current(state, ref);
	if (!objective && !max_rounds)
		throw goal_error("goal edit requires objective and/or maxGoalRounds", goal_error_code::invalid_edit);
	goal_snapshot goal = *state.current;
	goal.revision += 1;
	if (objective) {
		auto normalized = trim(*objective);
		if (normalized.empty())
			throw goal_error("goal objective must be a non-empty string", goal_error_code::invalid_objective);
		goal.objective = normalized;
	}
	if (max_rounds) {
		if (*max_rounds < 1)
			throw goal_error("maxGoalRounds must be a positive integer", goal_error_code::invalid_max_rounds);
		goal.max_goal_rounds = *max_rounds;
	}
	return commit_current(session, "edit", goal, activation_of(session));
}

namespace {

goal_snapshot with_phase(goal_snapshot current, goal_phase phase) {
	current.revision += 1;
	current.phase = phase;
	current.blocked_reason.reset();
	return current;
}

} // namespace

goal_view goal_service::pause(araya::session::session_id const& session, goal_ref const& ref) {
	ensure_session(session);
	auto const& state = expect_state(session);
	expect_current(state, ref);
	if (state.current->phase != goal_phase::active)
		throw goal_error("cannot pause goal from its phase", goal_error_code::invalid_transition);
	return commit_current(session, "pause", with_phase(*state.current, goal_phase::paused), goal_activation::disarmed);
}

goal_view goal_service::resume(araya::session::session_id const& session, goal_ref const& ref) {
	ensure_session(session);
	auto const& state = expect_state(session);
	expect_current(state, ref);
	auto const& current = *state.current;
	if (current.phase != goal_phase::active && current.phase != goal_phase::paused &&
		current.phase != goal_phase::blocked)
		throw goal_error("cannot resume goal from its phase", goal_error_code::invalid_transition);
	if (current.phase == goal_phase::active && activation_of(session) == goal_activation::armed)
		throw goal_error("goal is already active and armed", goal_error_code::invalid_transition);
	if (state.rounds_started >= current.max_goal_rounds)
		throw goal_error("goal exhausted its round budget", goal_error_code::invalid_transition);
	return commit_current(session, "resume", with_phase(current, goal_phase::active), goal_activation::armed);
}

goal_view goal_service::complete(araya::session::session_id const& session, goal_ref const& ref) {
	ensure_session(session);
	auto const& state = expect_state(session);
	expect_current(state, ref);
	if (state.current->phase == goal_phase::complete)
		throw goal_error("goal is already complete", goal_error_code::invalid_transition);
	return commit_current(
		session, "complete", with_phase(*state.current, goal_phase::complete), goal_activation::disarmed);
}

goal_view
goal_service::block(araya::session::session_id const& session, goal_ref const& ref, goal_block_reason reason) {
	ensure_session(session);
	auto const& state = expect_state(session);
	expect_current(state, ref);
	if (state.current->phase != goal_phase::active)
		throw goal_error("cannot block goal from its phase", goal_error_code::invalid_transition);
	auto code = trim(reason.code);
	auto message = trim(reason.message);
	if (code.empty() || message.empty())
		throw goal_error("block reason requires a code and a message", goal_error_code::invalid_block_reason);
	goal_snapshot goal = with_phase(*state.current, goal_phase::blocked);
	goal.blocked_reason = goal_block_reason{code, message};
	return commit_current(session, "block", goal, goal_activation::disarmed);
}

goal_ref goal_service::clear(araya::session::session_id const& session, goal_ref const& ref) {
	ensure_session(session);
	auto const& state = expect_state(session);
	expect_current(state, ref);
	goal_ref tombstone{state.current->id, state.current->revision + 1};
	boost::json::object change;
	change["kind"] = "goal/change";
	change["version"] = 1;
	change["operation"] = "clear";
	change["cleared"] = boost::json::object{{"id", tombstone.id}, {"revision", tombstone.revision}};
	change["clearedAt"] = std::max(araya::session::now_ms(), state.updated_at);
	commit_change(session, std::move(change), goal_activation::disarmed);
	return tombstone;
}

std::optional<goal_view> goal_service::get(araya::session::session_id const& session) const {
	auto state = projection_.state_of(session);
	if (!state)
		return std::nullopt;
	if (state->failure)
		throw goal_error(*state->failure, goal_error_code::invalid_transition);
	if (!state->current)
		return std::nullopt;
	return view_of(*state, activation_of(session));
}

std::optional<goal_view> goal_service::disarm(araya::session::session_id const& session) {
	auto state = projection_.state_of(session);
	if (!state || !state->current)
		return std::nullopt;
	set_activation(session, goal_activation::disarmed);
	return view_of(*state, goal_activation::disarmed);
}

goal_projection_state const* goal_service::state_of(araya::session::session_id const& session) const {
	return projection_.state_of(session);
}

} // namespace araya::goal
