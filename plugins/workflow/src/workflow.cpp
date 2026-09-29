#include "araya/workflow/workflow.hpp"

#include "araya/util/json.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace araya::workflow {
namespace {

namespace json = araya::util::json;
using araya::session::session;
using araya::session::session_id;

boost::json::value id_of(boost::json::value const& call) {
	auto const* object = call.if_object();
	if (!object)
		return nullptr;
	auto const* id = object->if_contains("id");
	return id ? *id : boost::json::value(nullptr);
}

void emit(progress_sink const& sink, std::string kind, boost::json::value data) {
	if (sink)
		sink(progress{std::move(kind), std::move(data)});
}

void record(session& target, std::string type, boost::json::value data) {
	target.append(std::move(type), std::move(data));
}

} // namespace

struct workflow_service::run_state {
	std::string id;
	std::string name;
	std::uint64_t agents_started = 0;
	std::uint64_t max_total = 0;
	std::uint64_t next_seq = 0;
	std::uint64_t inflight = 0;
	std::deque<boost::json::value> queue;
	std::shared_ptr<std::stop_source> run_stop;
	std::optional<boost::json::value> value;
	std::optional<std::string> error;
	bool done = false;
	bool cancelled = false;
	std::shared_ptr<worker> transport;
};

workflow_service::workflow_service(
	boost::asio::any_io_executor executor,
	std::shared_ptr<araya::session::session_store> store,
	std::shared_ptr<araya::subagents::subagents_service> subagents,
	config cfg)
	: executor_(std::move(executor))
	, store_(std::move(store))
	, subagents_(std::move(subagents))
	, config_(std::move(cfg))
	, factory_([] { return std::shared_ptr<worker>{}; }) {}

void workflow_service::set_worker_factory(worker_factory factory) { factory_ = std::move(factory); }

araya::task<result> workflow_service::run(start_request request, progress_sink on_progress) {
	// Confine the body to the control strand for its whole lifetime (not just
	// the first hop): session appends from the pump path must dispatch there.
	auto self = shared_from_this();
	co_return co_await boost::asio::co_spawn(
		executor_,
		[self, request = std::move(request), on_progress = std::move(on_progress)]() mutable -> araya::task<result> {
			co_return co_await self->run_impl(std::move(request), std::move(on_progress));
		},
		boost::asio::use_awaitable);
}

araya::task<result> workflow_service::run_impl(start_request request, progress_sink on_progress) {
	result r;
	auto session = store_->get(session_id{request.parent});
	if (!session) {
		r.stop_reason = "error";
		r.error = "workflow: unknown parent session '" + request.parent + "'";
		co_return r;
	}
	auto const* meta = request.meta.if_object();
	std::string const name = meta ? json::get_string(*meta, "name") : std::string{};
	if (name.empty()) {
		r.stop_reason = "error";
		r.error = "workflow: meta.name is required";
		co_return r;
	}
	if (meta) {
		std::string const description = json::get_string(*meta, "description");
		if (description.empty()) {
			r.stop_reason = "error";
			r.error = "workflow: meta.description is required";
			co_return r;
		}
	}

	auto state = std::make_shared<run_state>();
	state->id = "workflow-" + std::to_string(next_run_++);
	state->name = name;
	state->max_total = request.max_total_agents ? *request.max_total_agents : config_.max_total_agents;
	state->run_stop = std::make_shared<std::stop_source>();
	try {
		state->transport = factory_();
	} catch (std::exception const& e) {
		r.stop_reason = "error";
		r.error = "workflow: could not start the Node runtime (\"" + config_.node_executable + "\"): " + e.what() +
				  " - install Node.js or set the workflow plugin's `node_executable`";
		co_return r;
	}
	if (!state->transport) {
		r.stop_reason = "error";
		r.error = "workflow: no worker transport is configured";
		co_return r;
	}
	live_workers_.push_back(state->transport);

	record(*session, "tool-workflow/run-start", {{"runId", state->id}, {"name", name}});
	emit(on_progress, "run-start", {{"runId", state->id}, {"name", name}});

	std::shared_ptr<std::stop_callback<std::function<void()>>> bridge;
	if (request.stop.stop_possible()) {
		bridge = std::make_shared<std::stop_callback<std::function<void()>>>(request.stop, [state] {
			state->cancelled = true;
			if (state->run_stop)
				state->run_stop->request_stop();
			if (state->transport)
				state->transport->kill();
		});
	}

	try {
		state->transport->send(boost::json::value{
			{"type", "boot"}, {"data", {{"script", request.script}, {"args", request.args}, {"meta", request.meta}}}});
		auto self = shared_from_this();
		co_await state->transport->pump([self, state, session, on_progress](boost::json::value message) {
			self->handle_message(state, session, on_progress, std::move(message));
		});
	} catch (std::exception const& e) {
		state->error = e.what();
	}
	bridge.reset();
	if (state->run_stop)
		state->run_stop->request_stop();
	std::erase_if(live_workers_, [&](std::shared_ptr<worker> const& live) { return live == state->transport; });

	if (state->cancelled) {
		r.stop_reason = "cancelled";
	} else if (state->error) {
		r.stop_reason = "error";
		r.error = state->error;
	} else if (state->done) {
		r.stop_reason = "completed";
		r.value = state->value.value_or(boost::json::value(nullptr));
	} else {
		r.stop_reason = "error";
		r.error = "workflow: worker exited without a result";
	}
	r.agents_started = state->agents_started;

	record(
		*session,
		"tool-workflow/run-end",
		{{"runId", state->id}, {"stopReason", r.stop_reason}, {"agentsStarted", r.agents_started}});
	emit(on_progress, "run-end", {{"runId", state->id}, {"stopReason", r.stop_reason}});
	co_return r;
}

void workflow_service::handle_message(
	state_ptr const& state,
	session_ptr const& session,
	progress_sink const& on_progress,
	boost::json::value message) {
	auto const* object = message.if_object();
	if (!object)
		return;
	std::string const type = json::get_string(*object, "type");
	if (type == "call") {
		handle_call(state, session, on_progress, std::move(message));
		return;
	}
	if (type == "log") {
		emit(on_progress, "log", {{"text", json::get_string(*object, "text")}});
		return;
	}
	if (type == "phase") {
		emit(on_progress, "phase", {{"title", json::get_string(*object, "title")}});
		return;
	}
	if (type == "done") {
		state->done = true;
		if (auto const* error = object->if_contains("error")) {
			auto const* error_object = error->if_object();
			std::string message = error_object ? json::get_string(*error_object, "message") : std::string{};
			if (message.empty())
				message = "workflow: script failed";
			state->error = std::move(message);
		}
		if (auto const* value = object->if_contains("value"))
			state->value = *value;
		else
			state->value = boost::json::value(nullptr);
	}
}

void workflow_service::handle_call(
	state_ptr const& state,
	session_ptr const& session,
	progress_sink const& on_progress,
	boost::json::value call) {
	if (state->agents_started >= state->max_total) {
		send_error_reply(state, id_of(call), "workflow: agent cap of " + std::to_string(state->max_total) + " reached");
		return;
	}
	state->agents_started++;
	state->queue.push_back(std::move(call));
	pump_calls(state, session, on_progress);
}

void workflow_service::pump_calls(
	state_ptr const& state,
	session_ptr const& session,
	progress_sink const& on_progress) {
	while (state->inflight < config_.max_concurrency && !state->queue.empty()) {
		auto call = std::move(state->queue.front());
		state->queue.pop_front();
		auto const* object = call.if_object();
		auto const* args = object ? object->if_contains("args") : nullptr;
		auto const* args_object = args ? args->if_object() : nullptr;
		if (!args_object) {
			send_error_reply(state, id_of(call), "workflow: agent call needs an 'args' object");
			continue;
		}
		araya::subagents::start_request request;
		request.prompt = json::get_string(*args_object, "prompt");
		if (request.prompt.empty()) {
			send_error_reply(state, id_of(call), "workflow: agent call needs a non-empty 'prompt'");
			continue;
		}
		request.parent = session->id().value;
		auto const label = json::get_string(*args_object, "label");
		request.label = label;
		auto const provider = json::get_string(*args_object, "provider");
		if (!provider.empty())
			request.provider = provider;
		auto const model = json::get_string(*args_object, "model");
		if (!model.empty())
			request.model = model;
		if (auto const* schema = args_object->if_contains("schema"))
			request.output_schema = *schema;
		request.stop = state->run_stop->get_token();

		auto const seq = ++state->next_seq;
		std::string display = label;
		if (display.empty())
			display = request.prompt.substr(0, std::min<std::size_t>(request.prompt.size(), 60));
		record(
			*session,
			"tool-workflow/agent-start",
			{{"runId", state->id},
			 {"seq", seq},
			 {"label", display},
			 {"phase", json::get_string(*args_object, "phase")}});
		emit(
			on_progress,
			"agent-start",
			{{"runId", state->id},
			 {"seq", seq},
			 {"label", display},
			 {"phase", json::get_string(*args_object, "phase")}});

		state->inflight++;
		auto self = shared_from_this();
		boost::asio::co_spawn(
			executor_,
			[self, state, session, on_progress, call = std::move(call), request = std::move(request), seq]() mutable
				-> araya::task<void> {
				co_await self->finish_agent_call(state, session, on_progress, std::move(call), std::move(request), seq);
			},
			boost::asio::detached);
	}
}

araya::task<void> workflow_service::finish_agent_call(
	state_ptr const& state,
	session_ptr const& session,
	progress_sink const& on_progress,
	boost::json::value call,
	araya::subagents::start_request request,
	std::uint64_t seq) {
	bool const structured = request.output_schema.has_value();
	araya::subagents::result child;
	try {
		// `request.provider` is the child's LLM route override, not the
		// subagent provider: orchestration always spawns via `spawn`.
		child = co_await subagents_->run("spawn", std::move(request));
	} catch (std::exception const& e) {
		child.is_error = true;
		child.error = e.what();
	}

	if (child.is_error) {
		// A failed child resolves to null so the script can filter it.
		send_reply(state, id_of(call), boost::json::value(nullptr));
	} else if (structured) {
		send_reply(state, id_of(call), child.structure.value_or(boost::json::value(nullptr)));
	} else {
		send_reply(state, id_of(call), boost::json::value(child.output));
	}

	record(
		*session,
		"tool-workflow/agent-end",
		{{"runId", state->id},
		 {"seq", seq},
		 {"outcome", child.is_error ? "failed" : "completed"},
		 {"childId", child.child}});
	emit(
		on_progress,
		"agent-end",
		{{"runId", state->id}, {"seq", seq}, {"outcome", child.is_error ? "failed" : "completed"}});

	state->inflight--;
	pump_calls(state, session, on_progress);
	co_return;
}

void workflow_service::send_reply(state_ptr const& state, boost::json::value id, boost::json::value value) {
	if (!state->transport)
		return;
	state->transport->send({{"type", "reply"}, {"id", std::move(id)}, {"ok", true}, {"value", std::move(value)}});
}

void workflow_service::send_error_reply(state_ptr const& state, boost::json::value id, std::string message) {
	if (!state->transport)
		return;
	state->transport->send({{"type", "reply"}, {"id", std::move(id)}, {"ok", false}, {"message", std::move(message)}});
}

void workflow_service::shutdown() {
	for (auto const& worker : live_workers_) {
		if (worker)
			worker->kill();
	}
	live_workers_.clear();
}

} // namespace araya::workflow
