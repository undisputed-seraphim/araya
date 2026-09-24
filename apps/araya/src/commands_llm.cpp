#include "commands.hpp"

#include "araya/agent-loop/agent.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/llm/llm.hpp"
#include "araya/session/store.hpp"
#include "araya/util/overloaded.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <variant>

// The llm-facing commands: chat (one-shot through the seam), ask (the
// agent loop), and tool (the registered-tools listing).
namespace araya::app {
namespace {

using araya::session::session_store;
using araya::session::sessions_key;
using araya::util::overloaded;

char const* run_status_name(araya::agent::run_status status) {
	switch (status) {
	case araya::agent::run_status::completed:
		return "completed";
	case araya::agent::run_status::max_tokens:
		return "max_tokens";
	case araya::agent::run_status::aborted:
		return "aborted";
	case araya::agent::run_status::error:
		return "error";
	case araya::agent::run_status::blocked:
		return "blocked";
	}
	return "?";
}

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

		// Auto-create a session when none is current.
		std::shared_ptr<araya::session::session> s;
		if (ctx.current)
			s = store->get(*ctx.current);
		if (!s) {
			auto sid = store->mint_id();
			araya::session::create_session_options options;
			options.cwd = ctx.cwd;
			s = store->create(root_ctx, sid, std::move(options));
			ctx.current = sid;
			out("session: created " + sid.value);
		}

		auto seq = s->append(
			"user/message", araya::llm_bridge::user_message_data("u" + std::to_string(s->log().size()), text));
		out("session: appended user seq " + std::to_string(seq));
		// The user turn is durable before the model runs.
		co_await s->flush();

		// The first registered provider route wins (with both mounted,
		// "mock" sorts before "openai", which is what the tests want).
		auto provider = service->first_provider();
		if (!provider) {
			out("chat: llm service has no provider routes (load llm-mock or configure llm-openai)");
			co_return;
		}
		auto model = service->resolve_model(*provider, "");
		if (!model || model->model.empty()) {
			out("chat: provider '" + std::string(*provider) + "' did not resolve a default model");
			co_return;
		}

		araya::llm::generate_options options;
		options.provider = *provider;
		options.model = model->model;
		options.session_id = ctx.current->value;
		for (auto const& message : s->surface().messages())
			options.messages.push_back(araya::llm_bridge::to_llm_message(message));

		std::string assembled;
		araya::llm::token_usage usage;
		std::optional<boost::json::value> replay;
		std::optional<araya::llm::llm_failure> failure;
		auto why = araya::llm::finish_chunk::reason::stop;
		araya::llm::chunk_sink sink = [&](araya::llm::stream_chunk const& chunk) -> araya::task<void> {
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
			co_return;
		};
		co_await service->stream(options, sink);

		// The routing name and the human message. A future surface that
		// wants the provider's verbatim code (e.g. "content_filter") can
		// switch the name to failure->code_string() here.
		if (failure)
			out("chat: " + std::string(araya::llm::llm_error::code_name(failure->code)) + ": " + failure->message);
		if (assembled.empty()) {
			co_return;
		}
		auto seq_out = s->append(
			"assistant/message",
			araya::llm_bridge::assistant_message_data(
				"a" + std::to_string(s->log().size()), {araya::llm::text_block{assembled}}, usage, std::move(replay)));
		co_await s->flush();
		out("assistant: " + assembled);
		out("chat: tokens in " + std::to_string(usage.input_tokens) + " out " + std::to_string(usage.output_tokens) +
			" (assistant seq " + std::to_string(seq_out) + ")");
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

		std::shared_ptr<araya::session::session> s;
		if (ctx.current)
			s = store->get(*ctx.current);
		if (!s) {
			auto sid = store->mint_id();
			araya::session::create_session_options options;
			options.cwd = ctx.cwd;
			s = store->create(root_ctx, sid, std::move(options));
			ctx.current = sid;
			out("session: created " + sid.value);
		}

		auto provider = service->first_provider();
		if (!provider) {
			out("ask: llm service has no provider routes (load llm-mock or configure llm-openai)");
			co_return;
		}
		auto model = service->resolve_model(*provider, "");
		if (!model || model->model.empty()) {
			out("ask: provider '" + std::string(*provider) + "' did not resolve a default model");
			co_return;
		}

		araya::agent::run_options options;
		options.provider = *provider;
		options.model = model->model;
		options.session = s->id();
		options.input = text;

		std::string assembled;
		araya::agent::event_sink observer = [&](araya::agent::agent_event const& event) {
			std::visit(
				overloaded{
					[&](araya::llm::stream_chunk const& chunk) {
						std::visit(
							overloaded{
								[&](araya::llm::text_delta_chunk const& delta) { assembled += delta.text; },
								[](auto const&) {}},
							chunk);
					},
					[&](araya::agent::tool_call_event const& call) { out("tool: " + call.name); },
					[&](araya::agent::tool_done_event const& done) {
						out(std::string("tool: ") + done.name + (done.is_error ? " (error)" : " done"));
					},
					[](auto const&) {}},
				event);
		};
		auto outcome = co_await agent->run(options, observer);
		// The agent loop owns its session appends; make the whole turn
		// durable once it settles.
		co_await s->flush();

		if (outcome.failure)
			out("ask: " + std::string(araya::llm::llm_error::code_name(outcome.failure->code)) + ": " +
				outcome.failure->message);
		if (!assembled.empty())
			out("assistant: " + assembled);
		out(std::string("ask: ") + run_status_name(outcome.status));
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

		auto root_ctx = ctx.rt->root_context();
		auto store = root_ctx.require<session_store>(sessions_key).shared();

		// Auto-create a session when none is current, the same way chat
		// and ask do: a typed line always lands somewhere. The header
		// records the working directory.
		std::shared_ptr<araya::session::session> s;
		if (ctx.current)
			s = store->get(*ctx.current);
		if (!s) {
			auto sid = store->mint_id();
			araya::session::create_session_options options;
			options.cwd = ctx.cwd;
			s = store->create(root_ctx, sid, std::move(options));
			ctx.current = sid;
			out("session: created " + sid.value);
		}

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
		auto root_ctx = ctx.rt->root_context();
		auto agent = root_ctx.require<araya::agent::agent_service>(araya::agent::agent_key).shared();
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
