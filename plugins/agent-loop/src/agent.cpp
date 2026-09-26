#include "araya/agent-loop/agent.hpp"

#include "araya/llm/bridge.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/use_promise.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::agent {

namespace {

using araya::llm::block_start_chunk;
using araya::llm::content_block;
using araya::llm::content_block_type;
using araya::llm::finish_chunk;
using araya::llm::llm_failure;
using araya::llm::reasoning_block;
using araya::llm::reasoning_delta_chunk;
using araya::llm::stream_chunk;
using araya::llm::text_block;
using araya::llm::text_delta_chunk;
using araya::llm::token_usage;
using araya::llm::tool_call_block;
using araya::llm::tool_call_delta_chunk;
using araya::llm_bridge::assistant_message_data;
using araya::llm_bridge::system_message_data;
using araya::llm_bridge::to_llm_message;
using araya::llm_bridge::tool_result_data;
using araya::llm_bridge::user_message_data;
using araya::session::session;
using araya::tools::tool_context;
using araya::tools::tool_result;

void emit(event_sink const& sink, agent_event event) {
	if (sink)
		sink(std::move(event));
}

// Folds one model stream into the committed blocks, mirroring the wire
// translators' block discipline: block_end carries the assembled block,
// and interrupted open blocks close from their partial deltas.
struct step_accumulator {
	struct open_block {
		content_block_type kind = content_block_type::text;
		std::string text;
		std::string call_id;
		std::string name;
	};

	void reset() {
		closed.clear();
		open.clear();
		usage.reset();
		failure.reset();
		replay.reset();
		why = finish_chunk::reason::stop;
	}

	void push(stream_chunk const& chunk) {
		if (auto const* start = std::get_if<block_start_chunk>(&chunk)) {
			open.emplace(start->index, open_block{start->type, {}, {}, {}});
		} else if (auto const* delta = std::get_if<text_delta_chunk>(&chunk)) {
			open[delta->index].text += delta->text;
		} else if (auto const* delta = std::get_if<reasoning_delta_chunk>(&chunk)) {
			open[delta->index].text += delta->text;
		} else if (auto const* delta = std::get_if<tool_call_delta_chunk>(&chunk)) {
			auto& block = open[delta->index];
			block.kind = content_block_type::tool_call;
			if (!delta->id.empty())
				block.call_id = delta->id;
			if (!delta->name.empty())
				block.name = delta->name;
			block.text += delta->arguments_delta;
		} else if (auto const* end = std::get_if<araya::llm::block_end_chunk>(&chunk)) {
			open.erase(end->index);
			closed.push_back(end->block);
		} else if (auto const* usage_chunk = std::get_if<araya::llm::usage_chunk>(&chunk)) {
			usage = usage_chunk->usage;
		} else if (auto const* finish = std::get_if<finish_chunk>(&chunk)) {
			why = finish->why;
			failure = finish->failure;
			replay = finish->replay_state;
		}
	}

	// The blocks to commit: closed blocks, or closed plus the partial
	// open ones when the stream was interrupted (the abort story: delivered
	// text is finalized, never dropped).
	std::vector<content_block> delivered_blocks() const {
		auto result = closed;
		for (auto const& [index, block] : open) {
			switch (block.kind) {
			case content_block_type::text:
				result.emplace_back(text_block{block.text});
				break;
			case content_block_type::reasoning:
				result.emplace_back(reasoning_block{block.text});
				break;
			case content_block_type::tool_call:
				result.emplace_back(tool_call_block{block.call_id, block.name, block.text});
				break;
			case content_block_type::tool_result:
				break;
			}
		}
		return result;
	}

	std::vector<content_block> closed;
	std::map<std::size_t, open_block> open;
	std::optional<token_usage> usage;
	std::optional<llm_failure> failure;
	std::optional<boost::json::value> replay;
	finish_chunk::reason why = finish_chunk::reason::stop;
};

// Indexed by run_status; keep in enum order.
inline constexpr std::array<char const*, 5>
	k_run_status_names{"completed", "max_tokens", "aborted", "error", "blocked"};

boost::json::value attempt_data(std::uint64_t turn, std::uint64_t step, std::optional<llm_failure> failure) {
	boost::json::object data;
	data["turn"] = turn;
	data["step"] = step;
	if (failure)
		data["failure"] = araya::llm::failure_to_json(*failure);
	return data;
}

boost::json::value turn_end_data(std::uint64_t turn, run_status status, std::optional<llm_failure> failure) {
	boost::json::object data;
	data["turn"] = turn;
	data["reason"] = run_status_name(status);
	if (failure)
		data["failure"] = araya::llm::failure_to_json(*failure);
	return data;
}

boost::json::value tool_call_data(std::uint64_t turn, std::uint64_t step, tool_call_block const& call) {
	return {
		{"turn", turn},
		{"step", step},
		{"callId", call.id},
		{"name", call.name},
		{"arguments", call.arguments},
	};
}

tool_result error_result(std::string message) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(message)}}}, true};
}

std::vector<tool_call_block> tool_calls_of(std::vector<content_block> const& blocks) {
	std::vector<tool_call_block> calls;
	for (auto const& block : blocks) {
		if (auto const* call = std::get_if<tool_call_block>(&block))
			calls.push_back(*call);
	}
	return calls;
}

// Commits the step's model outcome: the assistant message (closed
// blocks, or the delivered partials when interrupted), or an attempt on
// failure. `terminal` is set when the step ends here; otherwise the
// returned blocks are scanned for tool calls.
struct step_commit {
	std::optional<run_status> terminal;
	std::vector<content_block> blocks;
};

step_commit
commit_model_outcome(session& session, std::uint64_t turn, std::uint64_t step, step_accumulator& accumulator) {
	auto const interrupted = accumulator.why == finish_chunk::reason::aborted;
	step_commit commit;
	commit.blocks = interrupted ? accumulator.delivered_blocks() : accumulator.closed;
	if (accumulator.failure && !interrupted) {
		// A provider failure is an attempt, never a message - even when
		// blocks were already delivered.
		session.append("assistant/attempt", attempt_data(turn, step, accumulator.failure));
		commit.terminal = run_status::error;
		return commit;
	}
	if (!commit.blocks.empty()) {
		session.append(
			"assistant/message",
			assistant_message_data(
				"a" + std::to_string(session.log().size()),
				commit.blocks,
				accumulator.usage.value_or(token_usage{}),
				accumulator.replay,
				turn,
				step,
				interrupted));
	} else if (interrupted) {
		// Aborted with nothing delivered: an attempt, not an empty message.
		session.append("assistant/attempt", attempt_data(turn, step, accumulator.failure));
	}
	if (interrupted) {
		commit.terminal = run_status::aborted;
	} else if (accumulator.why == finish_chunk::reason::error) {
		// An error finish that carried no failure is still an error.
		commit.terminal = run_status::error;
	} else if (accumulator.why == finish_chunk::reason::max_tokens) {
		commit.terminal = run_status::max_tokens;
	}
	return commit;
}

// One past the largest turn/start in the log (0 when the session never
// ran the loop).
std::uint64_t next_turn(session const& session) {
	std::uint64_t turn = 0;
	for (auto const& event : session.log()) {
		if (event.type != "turn/start")
			continue;
		auto const* object = event.data.if_object();
		if (!object)
			continue;
		auto const* node = object->if_contains("turn");
		if (!node)
			continue;
		if (node->is_int64())
			turn = std::max(turn, static_cast<std::uint64_t>(node->as_int64()));
		else if (node->is_uint64())
			turn = std::max(turn, node->as_uint64());
	}
	return turn + 1;
}

// Why a driver's lifecycle began: a seeded session is a resume, a fresh
// one a startup (the harness's SessionStartSource, trimmed).
boost::json::value lifecycle_source(session const& session) {
	return boost::json::object{{"kind", session.header().is_seeded ? "resume" : "startup"}};
}

void isolate_agent_keys(araya::context& scope, araya::session::session_id const& session) {
	for (auto key :
		 {agent_created_key.id,
		  agent_disposed_key.id,
		  agent_status_key.id,
		  agent_inbox_inserted_key.id,
		  agent_inbox_claimed_key.id,
		  agent_inbox_discarded_key.id,
		  agent_turn_stopping_key.id,
		  pre_step_key.id})
		scope.isolate(key, session.value);
}

std::uint64_t queue_size(inbox_state const& state, inbox_target target) {
	return static_cast<std::uint64_t>(
		target == inbox_target::next_turn ? state.next_turn.size() : state.next_step.size());
}

bool has_pending(inbox_state const& state) { return !state.next_turn.empty() || !state.next_step.empty(); }

} // namespace

char const* run_status_name(run_status status) noexcept {
	auto const index = std::to_underlying(status);
	return index < k_run_status_names.size() ? k_run_status_names[index] : "error";
}

// -- controller state ------------------------------------------------------

struct agent_service::agent_state {
	araya::session::session_id session;
	bool running = false;
	std::shared_ptr<std::stop_source> abort;
	bool wake_requested = false;
	std::shared_ptr<araya::context> realm_scope;
	drive_options exec;
	// One-shot `run` input appended directly on the drive's first step.
	std::optional<boost::json::value> seed;
	std::stop_token caller_stop;
	std::shared_ptr<std::stop_callback<std::function<void()>>> stop_bridge;
	std::vector<std::shared_ptr<std::move_only_function<void()>>> waiters;
	run_outcome last_outcome;
};

agent_service::agent_service(
	std::shared_ptr<araya::event_bus> bus,
	std::shared_ptr<araya::context> scope,
	boost::asio::any_io_executor executor,
	std::shared_ptr<araya::llm::llm_service> llm,
	std::shared_ptr<araya::session::session_store> store,
	std::shared_ptr<araya::system_prompt::system_prompt_service> prompts,
	std::shared_ptr<araya::tools::tools_service> tools,
	araya::session::projection_state<inbox_state> inbox,
	araya::session::projection_state<turn_state> turn_boundary)
	: bus_(std::move(bus))
	, scope_(std::move(scope))
	, executor_(std::move(executor))
	, llm_(std::move(llm))
	, store_(std::move(store))
	, prompts_(std::move(prompts))
	, tools_(std::move(tools))
	, inbox_(std::move(inbox))
	, turn_boundary_(std::move(turn_boundary)) {}

agent_service::state_ptr agent_service::find(araya::session::session_id const& session) const {
	auto it = states_.find(session);
	return it == states_.end() ? nullptr : it->second;
}

agent_service::state_ptr agent_service::require(araya::session::session_id const& session) const {
	auto state = find(session);
	if (!state)
		throw std::invalid_argument("agent: no driver for session '" + session.value + "'");
	return state;
}

// -- lifecycle -------------------------------------------------------------

araya::task<void> agent_service::ensure(araya::session::session const& session, boost::json::value source) {
	auto const id = session.id();
	if (states_.contains(id))
		co_return;
	auto state = std::make_shared<agent_state>();
	state->session = id;
	if (scope_) {
		state->realm_scope = scope_->make_child();
		isolate_agent_keys(*state->realm_scope, id);
	}
	states_.emplace(id, state);
	if (bus_) {
		try {
			co_await bus_->dispatch(
				agent_created_key, agent_created_msg{id, std::move(source)}, state->realm_scope.get());
		} catch (...) {
			states_.erase(id);
			throw;
		}
	}
}

araya::task<void> agent_service::dispose(araya::session::session_id const& session) {
	auto it = states_.find(session);
	if (it == states_.end())
		co_return;
	auto state = it->second;
	states_.erase(it);
	if (state->running && state->abort)
		state->abort->request_stop();
	auto waiters = std::move(state->waiters);
	state->waiters.clear();
	for (auto& waiter : waiters) {
		if (waiter && *waiter)
			(*waiter)();
	}
	if (bus_)
		co_await bus_->dispatch(agent_disposed_key, agent_disposed_msg{session}, state->realm_scope.get());
}

void agent_service::shutdown() {
	for (auto& [id, state] : states_) {
		if (state->running && state->abort)
			state->abort->request_stop();
		auto waiters = std::move(state->waiters);
		state->waiters.clear();
		for (auto& waiter : waiters) {
			if (waiter && *waiter)
				(*waiter)();
		}
	}
	states_.clear();
}

// -- driver ----------------------------------------------------------------

void agent_service::emit_status(state_ptr const& state, bool running) {
	if (state->running == running)
		return;
	state->running = running;
	if (bus_)
		bus_->dispatch(
			agent_status_key,
			agent_status_msg{state->session, running ? agent_status::running : agent_status::idle},
			state->realm_scope.get());
}

void agent_service::resolve_route(state_ptr const& state, session const& session) {
	if (!state->exec.provider.empty() && !state->exec.model.empty())
		return;
	for (auto it = session.log().rbegin(); it != session.log().rend(); ++it) {
		if (it->type != "request/header")
			continue;
		auto const* object = it->data.if_object();
		if (!object)
			continue;
		auto hit = object->find("header");
		if (hit == object->end() || !hit->value().is_object())
			continue;
		auto const& header = hit->value().as_object();
		auto pick = [&](std::string_view key) -> std::string {
			auto found = header.find(key);
			if (found == header.end() || !found->value().is_string())
				return {};
			return std::string(found->value().as_string());
		};
		if (state->exec.provider.empty())
			state->exec.provider = pick("provider");
		if (state->exec.model.empty())
			state->exec.model = pick("model");
		if (state->exec.reasoning_effort.empty())
			state->exec.reasoning_effort = pick("reasoning_effort");
		return;
	}
}

void agent_service::merge_exec(state_ptr const& state, drive_options const& options) {
	if (!options.provider.empty())
		state->exec.provider = options.provider;
	if (!options.model.empty())
		state->exec.model = options.model;
	if (!options.reasoning_effort.empty())
		state->exec.reasoning_effort = options.reasoning_effort;
	if (options.temperature)
		state->exec.temperature = options.temperature;
	if (options.max_tokens)
		state->exec.max_tokens = options.max_tokens;
	state->exec.max_steps = options.max_steps;
	if (options.on_event)
		state->exec.on_event = options.on_event;
	if (options.on_chunk)
		state->exec.on_chunk = options.on_chunk;
	if (options.stop.stop_possible())
		state->caller_stop = options.stop;
}

void agent_service::enqueue(state_ptr const& state, inbox_target target, boost::json::value const& message) {
	auto session = store_->get(state->session);
	if (!session)
		throw std::invalid_argument("agent: no entered session '" + state->session.value + "'");
	auto inbox = inbox_.state_of(state->session);
	if (!inbox)
		throw std::runtime_error("agent: session '" + state->session.value + "' has no inbox projection");
	auto mutation = make_inbox_splice(
		*inbox, target, std::numeric_limits<std::int64_t>::max(), 0, boost::json::array{message}, false);
	if (mutation.noop)
		return;
	session->append(std::string(k_inbox_spliced_event), mutation.data);
	if (bus_)
		bus_->dispatch(
			agent_inbox_inserted_key, agent_inbox_inserted_msg{state->session, message}, state->realm_scope.get());
}

void agent_service::clear_inbox(state_ptr const& state) {
	auto session = store_->get(state->session);
	if (!session)
		return;
	for (auto target : {inbox_target::next_step, inbox_target::next_turn}) {
		auto inbox = inbox_.state_of(state->session);
		if (!inbox || queue_size(*inbox, target) == 0)
			continue;
		auto mutation =
			make_inbox_splice(*inbox, target, 0, static_cast<std::int64_t>(queue_size(*inbox, target)), {}, true);
		if (mutation.noop)
			continue;
		session->append(std::string(k_inbox_spliced_event), mutation.data);
		for (auto const& message : mutation.removed) {
			if (bus_)
				bus_->dispatch(
					agent_inbox_discarded_key,
					agent_inbox_discarded_msg{state->session, message},
					state->realm_scope.get());
		}
	}
}

void agent_service::kick(state_ptr const& state) {
	if (state->running)
		return;
	auto session = store_->get(state->session);
	if (!session)
		return;
	resolve_route(state, *session);
	state->abort = std::make_shared<std::stop_source>();
	state->stop_bridge.reset();
	if (state->caller_stop.stop_possible()) {
		auto* raw = state->abort.get();
		state->stop_bridge = std::make_shared<std::stop_callback<std::function<void()>>>(
			state->caller_stop, [raw] { raw->request_stop(); });
	}
	state->exec.stop = state->abort->get_token();
	emit_status(state, true);
	auto self = shared_from_this();
	boost::asio::co_spawn(
		executor_,
		[self, state]() -> araya::task<void> { co_await self->drive(state); },
		[self, state](std::exception_ptr ep) { self->on_drive_done(state, ep); });
}

void agent_service::on_drive_done(state_ptr const& state, std::exception_ptr ep) {
	if (ep) {
		try {
			std::rethrow_exception(ep);
		} catch (std::exception const& e) {
			state->last_outcome.status = run_status::error;
			state->last_outcome.failure = llm_failure{araya::llm::llm_error_code::server, e.what()};
		} catch (...) {
			state->last_outcome.status = run_status::error;
			state->last_outcome.failure = llm_failure{araya::llm::llm_error_code::server, "agent: unknown failure"};
		}
	}
	state->stop_bridge.reset();
	state->caller_stop = std::stop_token{};
	state->exec.on_event = {};
	state->exec.on_chunk = {};
	emit_status(state, false);
	// Completing a waiter resumes it synchronously, and that run may start a
	// new drive (which registers a fresh waiter). Snapshot and clear first so
	// the resumed work cannot be wiped by this completion.
	auto waiters = std::move(state->waiters);
	state->waiters.clear();
	for (auto& waiter : waiters) {
		if (waiter && *waiter)
			(*waiter)();
	}
	if (state->wake_requested) {
		state->wake_requested = false;
		auto inbox = inbox_.state_of(state->session);
		if (inbox && has_pending(*inbox))
			kick(state);
	}
}

araya::task<void> agent_service::drive(state_ptr state) {
	while (!state->exec.stop.stop_requested()) {
		if (!co_await turn(state))
			break;
	}
}

araya::task<bool> agent_service::turn(state_ptr const& state) {
	auto session = store_->get(state->session);
	if (!session)
		co_return false;
	auto const turn_no = next_turn(*session);
	state->last_outcome.turn = turn_no;
	event_sink const& sink = state->exec.on_event;
	emit(sink, turn_event{turn_no});
	session->append("turn/start", boost::json::value{{"turn", turn_no}});

	std::optional<run_status> ended;
	std::uint64_t step = 0;
	bool first_step = true;

	for (;;) {
		if (state->exec.stop.stop_requested()) {
			ended = run_status::aborted;
			break;
		}
		if (step >= state->exec.max_steps) {
			ended = run_status::blocked;
			break;
		}
		++step;
		araya::system_prompt::prompt_assembly assembly;
		auto decision = co_await pre_step(state, *session, turn_no, step, first_step, assembly);
		if (decision.reject) {
			ended = run_status::blocked;
			break;
		}
		if (ended && decision.messages.empty())
			break;
		if (first_step && decision.messages.empty()) {
			ended = run_status::completed;
			break;
		}

		emit(sink, step_event{turn_no, step});
		session->append("step/start", boost::json::value{{"turn", turn_no}, {"step", step}});
		std::optional<run_status> exit;
		try {
			exit = co_await run_step(state, *session, turn_no, step, assembly, decision.messages, sink);
		} catch (...) {
			session->append("step/end", boost::json::value{{"turn", turn_no}, {"step", step}});
			throw;
		}
		session->append("step/end", boost::json::value{{"turn", turn_no}, {"step", step}});
		if (exit && (!ended || *ended != run_status::max_tokens))
			ended = exit;
		first_step = false;

		bool next_step_pending = false;
		if (auto inbox = inbox_.state_of(state->session))
			next_step_pending = !inbox->next_step.empty();
		if (ended && !next_step_pending) {
			if (bus_)
				co_await bus_->dispatch(
					agent_turn_stopping_key,
					agent_turn_stopping_msg{state->session, turn_no, state->exec.stop},
					state->realm_scope.get());
			if (auto inbox = inbox_.state_of(state->session))
				next_step_pending = !inbox->next_step.empty();
		}
		if (ended && !next_step_pending)
			break;
	}

	session->append(
		"turn/end", turn_end_data(turn_no, ended.value_or(run_status::completed), state->last_outcome.failure));
	state->last_outcome.status = ended.value_or(run_status::completed);
	emit(sink, run_finish_event{turn_no, state->last_outcome.status});

	if (state->exec.stop.stop_requested())
		co_return false;
	auto inbox = inbox_.state_of(state->session);
	co_return inbox&& has_pending(*inbox);
}

araya::task<pre_step_msg> agent_service::pre_step(
	state_ptr const& state,
	session& session,
	std::uint64_t turn,
	std::uint64_t step,
	bool first_step,
	araya::system_prompt::prompt_assembly& assembly) {
	std::vector<boost::json::value> claimed;
	auto claim = [&](inbox_target target, std::int64_t count) {
		auto inbox = inbox_.state_of(state->session);
		if (!inbox)
			return;
		auto mutation = make_inbox_splice(*inbox, target, 0, count, {}, false);
		if (mutation.noop)
			return;
		session.append(std::string(k_inbox_spliced_event), mutation.data);
		for (auto& message : mutation.removed) {
			if (bus_)
				bus_->dispatch(
					agent_inbox_claimed_key,
					agent_inbox_claimed_msg{state->session, message, turn},
					state->realm_scope.get());
			claimed.push_back(std::move(message));
		}
	};
	if (auto inbox = inbox_.state_of(state->session))
		claim(inbox_target::next_step, static_cast<std::int64_t>(inbox->next_step.size()));
	if (first_step)
		claim(inbox_target::next_turn, 1);

	assembly = assemble_prompt(session, state->exec);

	std::vector<boost::json::value> proposed;
	if (first_step) {
		auto snapshot = araya::system_prompt::render_context_snapshot(assembly);
		if (!snapshot.empty())
			proposed.push_back(user_message_data("c" + std::to_string(session.log().size()), snapshot));
	}
	for (auto& message : claimed)
		proposed.push_back(std::move(message));
	if (first_step && state->seed) {
		proposed.push_back(std::move(*state->seed));
		state->seed.reset();
	}

	pre_step_msg message{state->session, turn, step, state->exec.stop, false, std::move(proposed), false};
	if (!bus_)
		co_return message;
	co_return co_await bus_->dispatch(pre_step_key, message, state->realm_scope.get());
}

araya::task<std::optional<run_status>> agent_service::run_step(
	state_ptr const& state,
	session& session,
	std::uint64_t turn,
	std::uint64_t step,
	araya::system_prompt::prompt_assembly const& prompt,
	std::vector<boost::json::value> const& messages,
	event_sink const& sink) {
	// The pre-stream half: project the prompt, admit the claimed batch,
	// and record the request header/context - in the harness's step order,
	// the system node folds before the admitted user messages.
	commit_system_prompt(session, araya::system_prompt::render_prompt(prompt));
	append_user_messages(session, messages);
	append_request_header(session, build_header(state->exec, prompt));
	append_request_context(session, state->exec);

	step_accumulator accumulator;
	araya::llm::chunk_sink chunk_sink = [&](stream_chunk const& chunk) -> araya::task<void> {
		// Forward the raw chunk by reference; the accumulator folds it.
		if (state->exec.on_chunk)
			co_await state->exec.on_chunk(chunk);
		accumulator.push(chunk);
		co_return;
	};
	co_await llm_->stream(build_generate(state->exec, session, prompt), chunk_sink);

	auto commit = commit_model_outcome(session, turn, step, accumulator);
	if (commit.terminal) {
		if (*commit.terminal == run_status::error)
			state->last_outcome.failure = accumulator.failure;
		co_return commit.terminal;
	}

	auto const calls = tool_calls_of(commit.blocks);
	if (calls.empty())
		co_return run_status::completed;
	if (co_await execute_tools(state, session, turn, step, calls, sink))
		co_return run_status::aborted;
	co_return std::nullopt;
}

araya::task<bool> agent_service::execute_tools(
	state_ptr const& state,
	session& session,
	std::uint64_t turn,
	std::uint64_t step,
	std::vector<tool_call_block> const& calls,
	event_sink const& sink) {
	auto const scope = session.id().value;
	bool aborted = false;
	for (auto const& call : calls) {
		session.append("tool/call", tool_call_data(turn, step, call));
		emit(sink, tool_call_event{turn, step, call.id, call.name});

		tool_result result;
		if (aborted || state->exec.stop.stop_requested()) {
			aborted = true;
			result = error_result("Error: tool call aborted before dispatch");
		} else {
			boost::system::error_code ec;
			auto arguments = boost::json::parse(call.arguments, ec);
			if (ec)
				arguments = boost::json::value{call.arguments};
			try {
				auto invoked = co_await tools_->invoke(
					call.name,
					tool_context{call.id, call.name, session.id().value, std::move(arguments), state->exec.stop},
					scope);
				result = invoked ? std::move(*invoked) : error_result("Error: unknown tool '" + call.name + "'");
			} catch (std::exception const& e) {
				result = error_result(std::string("Error: ") + e.what());
			}
		}

		session.append(
			"tool/result",
			tool_result_data("t" + std::to_string(session.log().size()), call.id, result.content, result.is_error));
		emit(sink, tool_done_event{turn, step, call.id, call.name, result.is_error});
	}
	co_return aborted;
}

// -- prompt/request assembly ----------------------------------------------

araya::system_prompt::prompt_assembly agent_service::assemble_prompt(session& session, drive_options const& options) {
	araya::system_prompt::assemble_context context;
	context.scope = session.id().value;
	context.provider = options.provider;
	context.model = options.model;
	if (session.header().cwd)
		context.cwd = *session.header().cwd;
	return prompts_->assemble(context);
}

boost::json::value
agent_service::build_header(drive_options const& options, araya::system_prompt::prompt_assembly const& prompt) const {
	boost::json::object header;
	header["provider"] = options.provider;
	header["model"] = options.model;
	if (!options.reasoning_effort.empty())
		header["reasoning_effort"] = options.reasoning_effort;
	if (options.max_tokens)
		header["max_tokens"] = *options.max_tokens;
	if (options.temperature)
		header["temperature"] = *options.temperature;
	boost::json::array tools;
	for (auto const& tool : prompt.tools)
		tools.emplace_back(tool.name);
	if (!tools.empty())
		header["tools"] = std::move(tools);
	return header;
}

araya::llm::generate_options agent_service::build_generate(
	drive_options const& options,
	session& session,
	araya::system_prompt::prompt_assembly const& prompt) const {
	araya::llm::generate_options generate;
	generate.provider = options.provider;
	generate.model = options.model;
	generate.reasoning_effort = options.reasoning_effort;
	generate.temperature = options.temperature;
	generate.max_tokens = options.max_tokens;
	generate.session_id = session.id().value;
	generate.stop_token = options.stop;
	for (auto const& message : session.surface().messages())
		generate.messages.push_back(to_llm_message(message));
	for (auto const& tool : prompt.tools)
		generate.tools.push_back(araya::llm::tool_schema{tool.name, tool.description, tool.parameters});
	return generate;
}

void agent_service::commit_system_prompt(session& session, std::string const& rendered) {
	if (rendered.empty())
		return; // no effective prompt: never write an empty system message
	// Append only when the effective agent-loop prompt changed. A change
	// mid-session lands after the cached history (the harness's in-history
	// update); an unchanged prompt adds nothing.
	std::optional<std::string> previous;
	for (auto it = session.surface().messages().rbegin(); it != session.surface().messages().rend(); ++it) {
		if (it->role == araya::session::message_role::system && it->source_plugin == "agent-loop") {
			previous = araya::llm_bridge::message_text(*it);
			break;
		}
	}
	if (previous && *previous == rendered)
		return;
	session.append(
		"system/message", system_message_data("s" + std::to_string(session.log().size()), "agent-loop", rendered));
}

void agent_service::append_user_messages(session& session, std::vector<boost::json::value> const& messages) {
	for (auto const& message : messages)
		session.append("user/message", message);
}

void agent_service::append_request_header(session& session, boost::json::value const& header) {
	std::optional<boost::json::value> last;
	for (auto it = session.log().rbegin(); it != session.log().rend(); ++it) {
		if (it->type == "request/header") {
			last = it->data;
			break;
		}
	}
	bool changed = !last;
	if (!changed) {
		auto const* object = last->if_object();
		auto const* found = object ? object->if_contains("header") : nullptr;
		changed = !found || *found != header;
	}
	if (changed)
		session.append(
			"request/header", boost::json::value{{"header", header}, {"reason", last ? "change" : "initial"}});
}

void agent_service::append_request_context(session& session, drive_options const& options) {
	boost::json::object context;
	context["provider"] = options.provider;
	context["model"] = options.model;
	if (auto model = llm_->resolve_model(options.provider, options.model); model && model->context_window > 0)
		context["contextWindow"] = model->context_window;

	std::optional<boost::json::value> last;
	for (auto it = session.log().rbegin(); it != session.log().rend(); ++it) {
		if (it->type == "request/context") {
			last = it->data;
			break;
		}
	}
	if (!last || *last != context)
		session.append("request/context", std::move(context));
}

// -- public driver API -----------------------------------------------------

araya::task<run_outcome>
agent_service::run(run_options const& options, event_sink const& sink, araya::llm::chunk_sink const& on_chunk) {
	auto session = store_->get(options.session);
	if (!session)
		throw std::invalid_argument("agent: no entered session '" + options.session.value + "'");
	co_await ensure(*session, lifecycle_source(*session));
	auto state = require(options.session);

	drive_options exec;
	exec.provider = options.provider;
	exec.model = options.model;
	exec.reasoning_effort = options.reasoning_effort;
	exec.temperature = options.temperature;
	exec.max_tokens = options.max_tokens;
	exec.max_steps = options.max_steps;
	exec.stop = options.stop;
	exec.on_event = sink;
	exec.on_chunk = on_chunk;
	merge_exec(state, exec);

	if (!options.input.empty()) {
		if (state->running) {
			// A drive is in flight: queue the explicit input durably rather
			// than seed a turn that has already opened.
			merge_exec(state, exec);
			enqueue(
				state,
				inbox_target::next_turn,
				user_message_data("u" + std::to_string(session->log().size()), options.input));
			state->wake_requested = true;
		} else {
			state->seed = user_message_data("u" + std::to_string(session->log().size()), options.input);
			kick(state);
		}
	} else {
		kick(state);
	}
	co_await when_idle(options.session);
	co_return state->last_outcome;
}

araya::task<void>
agent_service::followup(araya::session::session_id const& session, boost::json::value message, drive_options options) {
	auto s = store_->get(session);
	if (!s)
		throw std::invalid_argument("agent: no entered session '" + session.value + "'");
	co_await ensure(*s, lifecycle_source(*s));
	auto state = require(session);
	merge_exec(state, options);
	enqueue(state, inbox_target::next_turn, message);
	kick(state);
}

araya::task<void>
agent_service::steer(araya::session::session_id const& session, boost::json::value message, drive_options options) {
	auto s = store_->get(session);
	if (!s)
		throw std::invalid_argument("agent: no entered session '" + session.value + "'");
	co_await ensure(*s, lifecycle_source(*s));
	auto state = require(session);
	merge_exec(state, options);
	enqueue(state, inbox_target::next_step, message);
	kick(state);
}

araya::task<void> agent_service::inject(araya::session::session_id const& session, boost::json::value message) {
	auto s = store_->get(session);
	if (!s)
		throw std::invalid_argument("agent: no entered session '" + session.value + "'");
	co_await ensure(*s, lifecycle_source(*s));
	auto state = require(session);
	enqueue(state, inbox_target::next_step, message);
}

void agent_service::cancel(araya::session::session_id const& session, cancel_cause cause, bool keep_inbox) {
	(void)cause;
	auto state = find(session);
	if (!state)
		return;
	if (!keep_inbox)
		clear_inbox(state);
	if (state->running && state->abort)
		state->abort->request_stop();
}

agent_status agent_service::status(araya::session::session_id const& session) const {
	auto state = find(session);
	return state && state->running ? agent_status::running : agent_status::idle;
}

araya::task<void> agent_service::when_idle(araya::session::session_id const& session) {
	for (;;) {
		auto state = find(session);
		if (!state || !state->running)
			co_return;
		auto holder = std::make_shared<std::move_only_function<void()>>();
		boost::asio::experimental::use_promise_t<> token;
		// Copy (not move) the handler: use_promise calls make_promise() on
		// its own copy after initiation returns, so the object we store must
		// remain intact.
		auto gate = boost::asio::async_initiate<boost::asio::experimental::use_promise_t<>, void()>(
			boost::asio::bind_executor(executor_, [holder](auto const& handler) { *holder = handler; }), token);
		state->waiters.push_back(holder);
		co_await gate(boost::asio::use_awaitable);
	}
}

std::vector<araya::session::session_id> agent_service::roots() const {
	std::vector<araya::session::session_id> out;
	for (auto const& id : store_->list()) {
		auto session = store_->get(id);
		if (session && session->header().delegation_depth == 0)
			out.push_back(id);
	}
	return out;
}

inbox_state const* agent_service::inbox(araya::session::session_id const& session) const {
	return inbox_.state_of(session);
}

turn_state const* agent_service::turn_boundary(araya::session::session_id const& session) const {
	return turn_boundary_.state_of(session);
}

run_outcome agent_service::last_outcome(araya::session::session_id const& session) const {
	auto state = find(session);
	return state ? state->last_outcome : run_outcome{};
}

araya::context const* agent_service::scope_of(araya::session::session_id const& session) const {
	auto state = find(session);
	return state ? state->realm_scope.get() : nullptr;
}

} // namespace araya::agent
