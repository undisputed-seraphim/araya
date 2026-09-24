#include "commands.hpp"

#include "araya/llm/bridge.hpp"
#include "araya/persistence/persistence.hpp"
#include "araya/session/store.hpp"

#include <boost/json/serialize.hpp>

#include <cstddef>
#include <istream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// The session command: the store's surface, live for the CLI (new,
// switch, list, append, replace, fork, show, save, load, load-all,
// restore, close). One helper per subcommand keeps the dispatcher small
// and shares the store/backend lookup and the load-from-disk path.
namespace araya::app {
namespace {

using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;

std::shared_ptr<session_store> store_of(app_context& ctx) {
	return ctx.rt->root_context().require<session_store>(sessions_key).shared();
}

std::shared_ptr<araya::persistence::session_persistence> backend_of(app_context& ctx) {
	return ctx.rt->root_context()
		.require<araya::persistence::session_persistence>(araya::persistence::persistence_key)
		.shared();
}

// Loads one stored session into the store (prepare/enter/announce) and
// returns its event count, or nullopt when it is not on disk.
std::optional<std::size_t> load_stored(app_context& ctx, session_id const& target) {
	auto stored = backend_of(ctx)->read(target);
	if (!stored)
		return std::nullopt;
	auto const event_count = stored->events.size();
	auto store = store_of(ctx);
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
	return event_count;
}

void session_new(app_context& ctx, line_sink const& out, std::istream& is) {
	std::string id;
	is >> id;
	auto store = store_of(ctx);
	auto root_ctx = ctx.rt->root_context();
	session_id sid = id.empty() ? store->mint_id() : session_id{std::move(id)};
	auto s = store->create(root_ctx, std::move(sid), {});
	ctx.current = s->id();
	out("session: created " + ctx.current->value);
}

void session_switch(app_context& ctx, line_sink const& out, std::istream& is) {
	std::string id;
	is >> id;
	if (id.empty()) {
		out("session: switch needs an id");
		return;
	}
	auto store = store_of(ctx);
	session_id target{std::move(id)};
	if (!store->get(target)) {
		out("session: unknown id '" + target.value + "'");
		return;
	}
	ctx.current = std::move(target);
	out("session: switched to " + ctx.current->value);
}

void session_list(app_context& ctx, line_sink const& out) {
	auto ids = store_of(ctx)->list();
	if (ids.empty()) {
		out("session: none live");
		return;
	}
	for (auto const& id : ids) {
		bool is_current = ctx.current && ctx.current->value == id.value;
		out(std::string(is_current ? "* " : "  ") + id.value);
	}
}

void session_append(app_context& ctx, line_sink const& out, std::istream& is) {
	std::string text;
	std::getline(is, text);
	text = trim(text);
	if (!ctx.current) {
		out("session: no current session");
		return;
	}
	auto s = store_of(ctx)->get(*ctx.current);
	if (!s) {
		out("session: current session was disposed");
		ctx.current.reset();
		return;
	}
	auto seq =
		s->append("user/message", araya::llm_bridge::user_message_data("u" + std::to_string(s->log().size()), text));
	out("session: appended seq " + std::to_string(seq));
}

void session_replace(app_context& ctx, line_sink const& out, std::istream& is) {
	std::int64_t start = 0;
	std::int64_t end = 0;
	std::string text;
	is >> start >> end;
	std::getline(is, text);
	text = trim(text);
	if (!is || text.empty()) {
		out("session: usage: session replace <start> <end> <text>");
		return;
	}
	if (!ctx.current) {
		out("session: no current session");
		return;
	}
	auto s = store_of(ctx)->get(*ctx.current);
	if (!s) {
		out("session: current session was disposed");
		ctx.current.reset();
		return;
	}
	boost::json::value replace = {
		{"start_seq", start},
		{"end_seq", end},
		{"message", araya::llm_bridge::user_message_data("r" + std::to_string(s->log().size()), text)},
	};
	auto seq = s->append("surface/replace", std::move(replace));
	out("session: replaced [" + std::to_string(start) + ", " + std::to_string(end) + ") at seq " + std::to_string(seq));
}

void session_fork(app_context& ctx, line_sink const& out, std::istream& is) {
	std::string parent;
	std::string child;
	is >> parent >> child;
	if (parent.empty()) {
		out("session: usage: session fork <parent> [child]");
		return;
	}
	auto store = store_of(ctx);
	auto root_ctx = ctx.rt->root_context();
	session_id pid{std::move(parent)};
	session_id cid = child.empty() ? session_id{} : session_id{std::move(child)};
	auto s = store->fork(root_ctx, pid, std::move(cid));
	ctx.current = s->id();
	out("session: forked " + s->id().value + " from " + pid.value + " (" + std::to_string(s->inherited_event_count()) +
		" inherited events)");
}

void session_show(app_context& ctx, line_sink const& out) {
	if (!ctx.current) {
		out("session: no current session");
		return;
	}
	auto s = store_of(ctx)->get(*ctx.current);
	if (!s) {
		out("session: current session was disposed");
		ctx.current.reset();
		return;
	}
	for (auto const& m : s->surface().messages())
		out("  " + std::string(role_name(m.role)) + ": " + boost::json::serialize(m.content));
}

araya::task<void> session_save(app_context& ctx, line_sink const& out) {
	if (!ctx.current) {
		out("session: no current session");
		co_return;
	}
	auto s = store_of(ctx)->get(*ctx.current);
	if (!s) {
		out("session: current session was disposed");
		ctx.current.reset();
		co_return;
	}
	// The durability barrier: the persistence plugin's flush listener
	// drains and fsyncs before this completes.
	co_await s->flush();
	out("session: saved " + s->id().value);
}

void session_load(app_context& ctx, line_sink const& out, std::istream& is, bool all) {
	std::string id;
	is >> id;
	if (!all && id.empty()) {
		out("session: load needs an id (or use 'session load-all')");
		return;
	}
	auto store = store_of(ctx);
	std::vector<session_id> targets;
	if (all) {
		targets = backend_of(ctx)->list();
	} else {
		targets.push_back(session_id{std::move(id)});
	}
	if (targets.empty()) {
		out("session: nothing on disk");
		return;
	}
	for (auto const& target : targets) {
		if (store->get(target)) {
			out("session: " + target.value + " already live");
			continue;
		}
		auto count = load_stored(ctx, target);
		if (!count) {
			out("session: " + target.value + " not on disk");
			continue;
		}
		if (!ctx.current)
			ctx.current = target;
		out("session: restored " + target.value + " (" + std::to_string(*count) + " events)");
	}
}

void session_restore(app_context& ctx, line_sink const& out, std::istream& is) {
	// Make a stored session current: switch to it if it is already live,
	// otherwise load it from disk. This is the picker's action.
	std::string id;
	is >> id;
	if (id.empty()) {
		out("session: usage: session restore <id>");
		return;
	}
	auto store = store_of(ctx);
	session_id target{std::move(id)};
	if (store->get(target)) {
		ctx.current = target;
		out("session: switched to " + target.value);
		return;
	}
	auto count = load_stored(ctx, target);
	if (!count) {
		out("session: " + target.value + " not on disk");
		return;
	}
	ctx.current = target;
	out("session: restored " + target.value + " (" + std::to_string(*count) + " events)");
}

void session_close(app_context& ctx, line_sink const& out, std::istream& is) {
	std::string id;
	is >> id;
	if (id.empty() && !ctx.current) {
		out("session: no current session");
		return;
	}
	session_id target = id.empty() ? *ctx.current : session_id{std::move(id)};
	if (!store_of(ctx)->dispose(target)) {
		out("session: unknown id '" + target.value + "'");
		return;
	}
	if (ctx.current && ctx.current->value == target.value)
		ctx.current.reset();
	out("session: closed " + target.value);
}

} // namespace

araya::task<void> cmd_session(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		std::string sub;
		is >> cmd >> sub;

		if (sub == "new")
			session_new(ctx, out, is);
		else if (sub == "switch")
			session_switch(ctx, out, is);
		else if (sub == "list")
			session_list(ctx, out);
		else if (sub == "append")
			session_append(ctx, out, is);
		else if (sub == "replace")
			session_replace(ctx, out, is);
		else if (sub == "fork")
			session_fork(ctx, out, is);
		else if (sub == "show")
			session_show(ctx, out);
		else if (sub == "save")
			co_await session_save(ctx, out);
		else if (sub == "load")
			session_load(ctx, out, is, false);
		else if (sub == "load-all")
			session_load(ctx, out, is, true);
		else if (sub == "restore")
			session_restore(ctx, out, is);
		else if (sub == "close")
			session_close(ctx, out, is);
		else
			out("session: new | switch | list | append | replace | fork | show | save | load | load-all | restore | "
				"close");
	} catch (std::exception const& e) {
		out(std::string("session: ") + e.what());
	}
	co_return;
}

} // namespace araya::app
