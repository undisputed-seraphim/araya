#include "commands.hpp"

#include "araya/agent-loop/agent.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/llm/llm.hpp"
#include "araya/session/store.hpp"
#include "araya/util/overloaded.hpp"

#include <boost/json/value.hpp>

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// The llm-facing commands: chat (one-shot through the seam), ask (the
// agent loop), say (local capture), and tool (the registered-tools
// listing). The shared session/route/stream plumbing lives here.
namespace araya::app {
namespace {

using araya::session::session_store;
using araya::session::sessions_key;
using araya::util::overloaded;

// Auto-create a session when none is current: a typed line always lands
// somewhere, and the header records the working directory.
std::shared_ptr<araya::session::session> ensure_session(app_context& ctx, session_store& store, line_sink const& out) {
	if (ctx.current) {
		if (auto existing = store.get(*ctx.current))
			return existing;
	}
	auto sid = store.mint_id();
	araya::session::create_session_options options;
	options.cwd = ctx.cwd;
	auto root_ctx = ctx.rt->root_context();
	auto s = store.create(root_ctx, sid, std::move(options));
	ctx.current = sid;
	out("session: created " + sid.value);
	return s;
}

// The first registered provider route and its default model; `label`
// prefixes the failure messages (chat:/ask:). The first route wins (with
// both mounted, "mock" sorts before "openai").
struct route {
	std::string provider;
	araya::llm::model_info model;
};

std::optional<route> first_route(araya::llm::llm_service& service, line_sink const& out, std::string_view label) {
	auto provider = service.first_provider();
	if (!provider) {
		out(std::string(label) + ": llm service has no provider routes (load llm-mock or configure llm-openai)");
		return std::nullopt;
	}
	auto model = service.resolve_model(*provider, "");
	if (!model || model->model.empty()) {
		out(std::string(label) + ": provider '" + std::string(*provider) + "' did not resolve a default model");
		return std::nullopt;
	}
	return route{std::string(*provider), *model};
}

// Folds one model stream into the assembled text, usage, replay state,
// and terminal reason/failure - shared by chat's chunk sink and ask's
// agent-event observer.
struct text_collector {
	void feed(araya::llm::stream_chunk const& chunk) {
		std::visit(
			overloaded{
				[&](araya::llm::text_delta_chunk const& delta) { assembled += delta.text; },
				[&](araya::llm::usage_chunk const& u) { usage = u.usage; },
				[&](araya::llm::finish_chunk const& finish) {
					why = finish.why;
					failure = finish.failure;
					replay = finish.replay_state;
				},
				[](auto const&) {}},
			chunk);
	}

	std::string assembled;
	araya::llm::token_usage usage;
	std::optional<boost::json::value> replay;
	std::optional<araya::llm::llm_failure> failure;
	araya::llm::finish_chunk::reason why = araya::llm::finish_chunk::reason::stop;
};

} // namespace

araya::task<void> cmd_chat(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		is >> cmd;
		std::string text;
		std::getline(is, text);
		text = trim(text);
		if (text.empty()) {
			out("chat: usage: chat <text>");
			co_return;
		}

		auto root_ctx = ctx.rt->root_context();
		auto service = root_ctx.require<araya::llm::llm_service>(araya::llm::llm_key).shared();
		auto store = root_ctx.require<session_store>(sessions_key).shared();
		auto s = ensure_session(ctx, *store, out);

		auto seq = s->append(
			"user/message", araya::llm_bridge::user_message_data("u" + std::to_string(s->log().size()), text));
		out("session: appended user seq " + std::to_string(seq));
		// The user turn is durable before the model runs.
		co_await s->flush();

		auto resolved = first_route(*service, out, "chat");
		if (!resolved) {
			co_return;
		}

		araya::llm::generate_options options;
		options.provider = resolved->provider;
		options.model = resolved->model.model;
		options.session_id = ctx.current->value;
		for (auto const& message : s->surface().messages())
			options.messages.push_back(araya::llm_bridge::to_llm_message(message));

		text_collector collector;
		araya::llm::chunk_sink sink = [&](araya::llm::stream_chunk const& chunk) -> araya::task<void> {
			collector.feed(chunk);
			co_return;
		};
		co_await service->stream(options, sink);

		// The routing name and the human message. A future surface that
		// wants the provider's verbatim code (e.g. "content_filter") can
		// switch the name to failure->code_string() here.
		if (collector.failure)
			out("chat: " + std::string(araya::llm::llm_error::code_name(collector.failure->code)) + ": " +
				collector.failure->message);
		if (collector.assembled.empty())
			co_return;
		auto seq_out = s->append(
			"assistant/message",
			araya::llm_bridge::assistant_message_data(
				"a" + std::to_string(s->log().size()),
				{araya::llm::text_block{collector.assembled}},
				collector.usage,
				std::move(collector.replay)));
		co_await s->flush();
		out("assistant: " + collector.assembled);
		out("chat: tokens in " + std::to_string(collector.usage.input_tokens) + " out " +
			std::to_string(collector.usage.output_tokens) + " (assistant seq " + std::to_string(seq_out) + ")");
	} catch (std::exception const& e) {
		out(std::string("chat: ") + e.what());
	}
	co_return;
}

araya::task<void> cmd_ask(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		is >> cmd;
		std::string text;
		std::getline(is, text);
		text = trim(text);
		if (text.empty()) {
			out("ask: usage: ask <text>");
			co_return;
		}

		auto root_ctx = ctx.rt->root_context();
		auto store = root_ctx.require<session_store>(sessions_key).shared();
		auto service = root_ctx.require<araya::llm::llm_service>(araya::llm::llm_key).shared();
		auto agent = root_ctx.require<araya::agent::agent_service>(araya::agent::agent_key).shared();
		auto s = ensure_session(ctx, *store, out);

		auto resolved = first_route(*service, out, "ask");
		if (!resolved) {
			co_return;
		}

		araya::agent::run_options options;
		options.provider = resolved->provider;
		options.model = resolved->model.model;
		options.session = s->id();
		options.input = text;

		text_collector collector;
		araya::llm::chunk_sink on_chunk = [&](araya::llm::stream_chunk const& chunk) -> araya::task<void> {
			collector.feed(chunk);
			co_return;
		};
		araya::agent::event_sink on_event = [&](araya::agent::agent_event const& event) {
			std::visit(
				overloaded{
					[&](araya::agent::tool_call_event const& call) { out("tool: " + call.name); },
					[&](araya::agent::tool_done_event const& done) {
						out(std::string("tool: ") + done.name + (done.is_error ? " (error)" : " done"));
					},
					[](auto const&) {}},
				event);
		};
		auto outcome = co_await agent->run(options, on_event, on_chunk);
		// The agent loop owns its session appends; make the whole turn
		// durable once it settles.
		co_await s->flush();

		if (outcome.failure)
			out("ask: " + std::string(araya::llm::llm_error::code_name(outcome.failure->code)) + ": " +
				outcome.failure->message);
		if (!collector.assembled.empty())
			out("assistant: " + collector.assembled);
		out(std::string("ask: ") + araya::agent::run_status_name(outcome.status));
	} catch (std::exception const& e) {
		out(std::string("ask: ") + e.what());
	}
	co_return;
}

araya::task<void> cmd_say(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		is >> cmd;
		std::string text;
		std::getline(is, text);
		text = trim(text);
		if (text.empty()) {
			out("say: usage: say <text>");
			co_return;
		}

		auto store = ctx.rt->root_context().require<session_store>(sessions_key).shared();
		auto s = ensure_session(ctx, *store, out);

		// Local capture only - no llm service, no provider route. The
		// feed renders this as a user row. The flush makes the turn
		// durable now (the persistence barrier fsyncs), not just at exit.
		(void)s->append(
			"user/message", araya::llm_bridge::user_message_data("u" + std::to_string(s->log().size()), text));
		co_await s->flush();
	} catch (std::exception const& e) {
		out(std::string("say: ") + e.what());
	}
	co_return;
}

araya::task<void> cmd_tool(app_context& ctx, line_sink const& out, std::string const&) {
	try {
		auto agent = ctx.rt->root_context().require<araya::agent::agent_service>(araya::agent::agent_key).shared();
		auto tools = agent->tools();
		if (tools.empty()) {
			out("tool: no tools registered");
			co_return;
		}
		for (auto const& tool : tools)
			out("tool: " + tool.name + " - " + tool.description);
	} catch (std::exception const& e) {
		out(std::string("tool: ") + e.what());
	}
	co_return;
}

} // namespace araya::app
