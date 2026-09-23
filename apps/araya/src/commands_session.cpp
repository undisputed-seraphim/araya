#include "commands.hpp"

#include "araya/llm/bridge.hpp"
#include "araya/persistence/persistence.hpp"
#include "araya/session/store.hpp"

#include <boost/json/serialize.hpp>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

// The session command: the store's surface, live for the CLI (new,
// switch, list, append, replace, fork, show, save, load, close).
namespace araya::app {
namespace {

using araya::session::session_store;
using araya::session::sessions_key;

} // namespace

araya::task<void> cmd_session(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		std::string sub;
		is >> cmd >> sub;

		auto store_of = [&]() -> std::shared_ptr<session_store> {
			return ctx.rt->root_context().require<session_store>(sessions_key).shared();
		};

		if (sub == "new") {
			std::string id;
			is >> id;
			auto store = store_of();
			auto root_ctx = ctx.rt->root_context();
			araya::session::session_id sid = id.empty() ? store->mint_id() : araya::session::session_id{std::move(id)};
			(void)store->create(root_ctx, sid, {});
			ctx.current = sid;
			out("session: created " + sid.value);
		} else if (sub == "switch") {
			std::string id;
			is >> id;
			if (id.empty()) {
				out("session: switch needs an id");
			} else {
				auto store = store_of();
				araya::session::session_id target{id};
				if (!store->get(target)) {
					out("session: unknown id '" + target.value + "'");
				} else {
					ctx.current = std::move(target);
					out("session: switched to " + ctx.current->value);
				}
			}
		} else if (sub == "list") {
			auto store = store_of();
			auto ids = store->list();
			if (ids.empty()) {
				out("session: none live");
			} else {
				for (auto const& id : ids) {
					bool is_current = ctx.current && ctx.current->value == id.value;
					out(std::string(is_current ? "* " : "  ") + id.value);
				}
			}
		} else if (sub == "append") {
			std::string text;
			std::getline(is, text);
			text = trim(text);
			if (!ctx.current) {
				out("session: no current session");
			} else {
				auto store = store_of();
				auto s = store->get(*ctx.current);
				if (!s) {
					out("session: current session was disposed");
					ctx.current.reset();
				} else {
					auto seq = s->append(
						"user/message",
						araya::llm_bridge::user_message_data("u" + std::to_string(s->log().size()), text));
					out("session: appended seq " + std::to_string(seq));
				}
			}
		} else if (sub == "replace") {
			std::int64_t start = 0;
			std::int64_t end = 0;
			std::string text;
			is >> start >> end;
			std::getline(is, text);
			text = trim(text);
			if (!is || text.empty()) {
				out("session: usage: session replace <start> <end> <text>");
			} else if (!ctx.current) {
				out("session: no current session");
			} else {
				auto store = store_of();
				auto s = store->get(*ctx.current);
				if (!s) {
					out("session: current session was disposed");
					ctx.current.reset();
				} else {
					boost::json::value replace = {
						{"start_seq", start},
						{"end_seq", end},
						{"message", araya::llm_bridge::user_message_data("r" + std::to_string(s->log().size()), text)},
					};
					auto seq = s->append("surface/replace", std::move(replace));
					out("session: replaced [" + std::to_string(start) + ", " + std::to_string(end) + ") at seq " +
						std::to_string(seq));
				}
			}
		} else if (sub == "fork") {
			std::string parent;
			std::string child;
			is >> parent >> child;
			if (parent.empty()) {
				out("session: usage: session fork <parent> [child]");
			} else {
				auto store = store_of();
				auto root_ctx = ctx.rt->root_context();
				araya::session::session_id pid{parent};
				araya::session::session_id cid =
					child.empty() ? araya::session::session_id{} : araya::session::session_id{std::move(child)};
				auto s = store->fork(root_ctx, pid, cid);
				ctx.current = s->id();
				out("session: forked " + s->id().value + " from " + pid.value + " (" +
					std::to_string(s->inherited_event_count()) + " inherited events)");
			}
		} else if (sub == "show") {
			if (!ctx.current) {
				out("session: no current session");
			} else {
				auto store = store_of();
				auto s = store->get(*ctx.current);
				if (!s) {
					out("session: current session was disposed");
					ctx.current.reset();
				} else {
					for (auto const& m : s->surface().messages())
						out("  " + role_name(m.role) + ": " + boost::json::serialize(m.content));
				}
			}
		} else if (sub == "save") {
			if (!ctx.current) {
				out("session: no current session");
			} else {
				auto store = store_of();
				auto s = store->get(*ctx.current);
				if (!s) {
					out("session: current session was disposed");
					ctx.current.reset();
				} else {
					// The durability barrier: the persistence plugin's flush
					// listener drains and fsyncs before this completes.
					co_await s->flush();
					out("session: saved " + s->id().value);
				}
			}
		} else if (sub == "load" || sub == "load-all") {
			bool all = sub == "load-all";
			std::string id;
			is >> id;
			if (!all && id.empty()) {
				out("session: load needs an id (or use 'session load-all')");
			} else {
				auto store = store_of();
				auto backend =
					ctx.rt->root_context()
						.require<araya::persistence::session_persistence>(araya::persistence::persistence_key)
						.shared();
				std::vector<araya::session::session_id> targets;
				if (all) {
					targets = backend->list();
				} else {
					targets.push_back(araya::session::session_id{std::move(id)});
				}
				if (targets.empty()) {
					out("session: nothing on disk");
				} else {
					for (auto& target : targets) {
						if (store->get(target)) {
							out("session: " + target.value + " already live");
							continue;
						}
						auto stored = backend->read(target);
						if (!stored) {
							out("session: " + target.value + " not on disk");
							continue;
						}
						auto const event_count = stored->events.size();
						auto s = store->prepare(
							target,
							araya::session::create_session_options{
								.seed = std::move(stored->events),
								.inherited_event_count = stored->events.size(),
								.cwd = stored->header.cwd,
								.parent_session = stored->header.parent_session,
								.created_at = stored->header.created_at,
								.is_seeded = false,
								.origin = stored->header.origin,
								.delegation_depth = stored->header.delegation_depth,
								.agent_preset = stored->header.agent_preset,
							});
						store->enter(s);
						store->announce(*s);
						if (!ctx.current)
							ctx.current = target;
						out("session: restored " + target.value + " (" + std::to_string(event_count) + " events)");
					}
				}
			}
		} else if (sub == "close") {
			std::string id;
			is >> id;
			auto target = id.empty() ? *ctx.current : araya::session::session_id{std::move(id)};
			auto store = store_of();
			if (!store->dispose(target)) {
				out("session: unknown id '" + target.value + "'");
			} else {
				if (ctx.current && ctx.current->value == target.value)
					ctx.current.reset();
				out("session: closed " + target.value);
			}
		} else {
			out("session: new | switch | list | append | replace | fork | show | save | load | load-all | close");
		}
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
	co_return;
}

} // namespace araya::app
