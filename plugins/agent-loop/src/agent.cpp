#include "araya/agent-loop/agent.hpp"
#include "araya/llm/bridge.hpp"

#include <boost/json/parse.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace araya::agent {

namespace {

using araya::llm::block_start_chunk;
using araya::llm::content_block;
using araya::llm::content_block_type;
using araya::llm::finish_chunk;
using araya::llm::llm_error;
using araya::llm::llm_error_code;
using araya::llm::llm_failure;
using araya::llm::reasoning_block;
using araya::llm::reasoning_delta_chunk;
using araya::llm::stream_chunk;
using araya::llm::text_block;
using araya::llm::text_delta_chunk;
using araya::llm::token_usage;
using araya::llm::tool_call_block;
using araya::llm::tool_call_delta_chunk;
using araya::llm::usage_chunk;
using araya::llm_bridge::assistant_message_data;
using araya::llm_bridge::message_text;
using araya::llm_bridge::system_message_data;
using araya::llm_bridge::to_llm_message;
using araya::llm_bridge::tool_result_data;
using araya::llm_bridge::user_message_data;
using araya::session::session;

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

} // namespace

char const* run_status_name(run_status status) noexcept {
	auto const index = std::to_underlying(status);
	return index < k_run_status_names.size() ? k_run_status_names[index] : "error";
}

agent_service::agent_service(
	std::shared_ptr<araya::llm::llm_service> llm,
	std::shared_ptr<araya::session::session_store> store)
	: llm_(std::move(llm))
	, store_(std::move(store)) {}

void agent_service::set_system_prompt(std::string text) { system_prompt_ = std::move(text); }

araya::registration agent_service::register_tool(araya::plugin_context& caller, tool_spec spec, tool_handler handler) {
	if (spec.name.empty())
		throw std::invalid_argument("agent: tool name must not be empty");
	if (tools_.contains(spec.name))
		throw std::invalid_argument("agent: duplicate tool '" + spec.name + "'");
	auto name = spec.name;
	tools_.emplace(std::move(name), tool_entry{std::move(spec), std::move(handler)});
	return caller.effect([this, name]() -> araya::cleanup_action { return [this, name] { tools_.erase(name); }; });
}

araya::registration agent_service::register_system_prompt(
	araya::plugin_context& caller,
	std::string plugin,
	std::function<std::string()> render) {
	if (plugin.empty())
		throw std::invalid_argument("agent: system-prompt source must not be empty");
	sections_.push_back(prompt_section{std::move(plugin), std::move(render)});
	auto name = sections_.back().plugin;
	return caller.effect([this, name = std::string(name)]() -> araya::cleanup_action {
		return [this, name] {
			std::erase_if(sections_, [&](prompt_section const& section) { return section.plugin == name; });
		};
	});
}

std::vector<tool_spec> agent_service::tools() const {
	std::vector<tool_spec> result;
	result.reserve(tools_.size());
	for (auto const& [name, entry] : tools_)
		result.push_back(entry.spec);
	return result;
}

boost::json::value agent_service::build_header(run_options const& options) const {
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
	for (auto const& [name, entry] : tools_)
		tools.emplace_back(name);
	if (!tools.empty())
		header["tools"] = std::move(tools);
	return header;
}

araya::llm::generate_options agent_service::build_generate(run_options const& options, session& session) const {
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
	for (auto const& [name, entry] : tools_)
		generate.tools.push_back(
			araya::llm::tool_schema{entry.spec.name, entry.spec.description, entry.spec.parameters});
	return generate;
}

void agent_service::commit_system_prompt(session& session) {
	std::vector<std::pair<std::string, std::string>> components;
	if (!system_prompt_.empty())
		components.emplace_back("agent-loop", system_prompt_);
	for (auto const& section : sections_)
		components.emplace_back(section.plugin, section.render());

	for (auto const& [plugin, text] : components) {
		if (text.empty())
			continue;
		// Compare against the last system message the surface already
		// folds from this source: changed or absent commits, repeats are
		// skipped (runs must not duplicate the prompt).
		std::string last;
		bool present = false;
		for (auto it = session.surface().messages().rbegin(); it != session.surface().messages().rend(); ++it) {
			if (it->role == araya::session::message_role::system && it->source_plugin == plugin) {
				last = message_text(*it);
				present = true;
				break;
			}
		}
		if (present && last == text)
			continue;
		session.append("system/message", system_message_data("s" + std::to_string(session.log().size()), plugin, text));
	}
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

void agent_service::append_request_context(session& session, run_options const& options) {
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

void agent_service::open_step(
	session& session,
	run_options const& options,
	std::uint64_t turn,
	std::uint64_t step,
	event_sink const& sink) {
	emit(sink, step_event{turn, step});
	session.append("step/start", boost::json::value{{"turn", turn}, {"step", step}});

	if (step == 1) {
		// The first step boundary commits the system prompt and this
		// run's user input, in that order - the harness's fold order:
		// the system message precedes the user message it serves.
		commit_system_prompt(session);
		if (!options.input.empty())
			session.append(
				"user/message", user_message_data("u" + std::to_string(session.log().size()), options.input));
	}

	append_request_header(session, build_header(options));
	append_request_context(session, options);
}

araya::task<bool> agent_service::execute_tools(
	session& session,
	run_options const& options,
	std::uint64_t turn,
	std::uint64_t step,
	std::vector<tool_call_block> const& calls,
	event_sink const& sink) {
	bool aborted = false;
	for (auto const& call : calls) {
		session.append("tool/call", tool_call_data(turn, step, call));
		emit(sink, tool_call_event{turn, step, call.id, call.name});

		tool_result result;
		if (aborted || options.stop.stop_requested()) {
			// Abort drains the queue: every remaining call still gets its
			// durable pair so replay stays valid (the harness's synthetic
			// "aborted before dispatch" results).
			aborted = true;
			result = error_result("Error: tool call aborted before dispatch");
		} else if (auto found = tools_.find(call.name); found == tools_.end()) {
			result = error_result("Error: unknown tool '" + call.name + "'");
		} else {
			boost::system::error_code ec;
			auto arguments = boost::json::parse(call.arguments, ec);
			if (ec)
				arguments = boost::json::value{call.arguments};
			try {
				result = co_await found->second.handler(
					tool_context{call.id, call.name, std::move(arguments), options.stop});
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

araya::task<run_outcome>
agent_service::run(run_options const& options, event_sink const& sink, araya::llm::chunk_sink const& on_chunk) {
	auto session = store_->get(options.session);
	if (!session)
		throw std::invalid_argument("agent: no entered session '" + options.session.value + "'");
	if (options.provider.empty() || options.model.empty())
		throw std::invalid_argument("agent: run requires a provider and model");

	run_outcome outcome;
	outcome.turn = next_turn(*session);
	auto const turn = outcome.turn;

	emit(sink, turn_event{turn});
	session->append("turn/start", boost::json::value{{"turn", turn}});

	std::uint64_t step = 0;
	step_accumulator accumulator;

	auto run_step = [&]() -> araya::task<std::optional<run_status>> {
		open_step(*session, options, turn, step, sink);

		accumulator.reset();
		araya::llm::chunk_sink chunk_sink = [&](stream_chunk const& chunk) -> araya::task<void> {
			// Forward the raw chunk by reference; the accumulator folds it.
			// No event-variant wrapper, so no per-delta copy.
			if (on_chunk)
				co_await on_chunk(chunk);
			accumulator.push(chunk);
			co_return;
		};
		co_await llm_->stream(build_generate(options, *session), chunk_sink);

		auto commit = commit_model_outcome(*session, turn, step, accumulator);
		if (commit.terminal)
			co_return *commit.terminal;

		auto const calls = tool_calls_of(commit.blocks);
		if (calls.empty())
			co_return run_status::completed;
		if (co_await execute_tools(*session, options, turn, step, calls, sink))
			co_return run_status::aborted;
		co_return std::nullopt;
	};

	for (;;) {
		if (options.stop.stop_requested()) {
			outcome.status = run_status::aborted;
			break;
		}
		if (step >= options.max_steps) {
			outcome.status = run_status::blocked;
			break;
		}
		++step;
		auto exit_status = co_await run_step();
		session->append("step/end", boost::json::value{{"turn", turn}, {"step", step}});
		if (exit_status) {
			outcome.status = *exit_status;
			if (*exit_status == run_status::error)
				outcome.failure = accumulator.failure;
			break;
		}
	}

	session->append("turn/end", turn_end_data(turn, outcome.status, outcome.failure));
	emit(sink, run_finish_event{turn, outcome.status});
	co_return outcome;
}

} // namespace araya::agent
