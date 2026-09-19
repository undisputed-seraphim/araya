#include "araya/session/store.hpp"

#include "repair.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace araya::session {

session::session(
	session_header header,
	session_log_offset inherited_event_count,
	session_seq first_live_seq,
	std::vector<session_event> log,
	std::vector<message_projection> const& projections,
	std::weak_ptr<session_store> store)
	: header_(std::move(header))
	, inherited_event_count_(inherited_event_count)
	, first_live_seq_(first_live_seq)
	, next_seq_(log.size())
	, log_(std::move(log))
	, store_(std::move(store)) {
	for (auto const& ev : log_)
		fold(ev, projections);
}

void session::fold(session_event const& ev, std::vector<message_projection> const& projections) {
	auto plan = fold_event(ev, projections);
	switch (plan.action) {
	case surface_plan::kind::append:
		surface_.push(ev.seq, std::move(*plan.message));
		break;
	case surface_plan::kind::replace:
		surface_.replace(plan.start, plan.end, ev.seq, std::move(plan.message));
		break;
	case surface_plan::kind::none:
		break;
	}
}

session_seq session::append(std::string type, boost::json::value data) {
	session_event ev{next_seq_++, now_ms(), std::move(type), std::move(data), false};
	if (auto s = store_.lock()) {
		s->publish(*this, ev);
	} else {
		ev.ignorable = !is_builtin_surface_type(ev.type);
		fold(ev, {});
	}
	log_.push_back(std::move(ev));
	return log_.back().seq;
}

araya::task<void> session::flush() {
	auto s = store_.lock();
	if (!s)
		co_return;
	co_await s->flush(id());
}

session_store::session_store(plugin_context& owner)
	: bus_(owner.activation_ptr() ? owner.activation_ptr()->bus : nullptr) {}

// Store teardown (the sessions provider unloading) removes every entered
// session; each announced session gets its 'session/disposed' event. When
// the last reference drops inside the engine (unbinding runs on the
// control strand) the events dispatch normally; when the store dies with
// the runtime (harness teardown) the bus is already past caring, so
// notification is skipped.
session_store::~session_store() {
	if (!bus_ || !bus_->on_control_strand()) {
		store_.clear();
		return;
	}
	std::vector<session_id> ids;
	ids.reserve(store_.size());
	for (auto const& [id, s] : store_)
		ids.push_back(id);
	for (auto const& id : ids) {
		store_.erase(id);
		bus_->dispatch(disposed_key, session_disposed_msg{id});
	}
}

session_id session_store::mint_id() { return session_id{"session-" + std::to_string(counter_++)}; }

std::vector<session_id> session_store::list() const {
	std::vector<session_id> out;
	out.reserve(store_.size());
	for (auto const& [id, s] : store_)
		out.push_back(id);
	return out;
}

std::shared_ptr<session> session_store::create(plugin_context& caller, session_id id, create_session_options options) {
	auto s = prepare(std::move(id), std::move(options));
	enter(s);
	// The calling fiber owns the session: unloading it stops notification
	// and removes the entry, an ordinary tracked effect like any other
	// plugin resource. The weak capture keeps the cleanup safe if the
	// store dies first.
	std::weak_ptr<session_store> weak = weak_from_this();
	session_id sid = s->id();
	(void)caller.effect([weak, sid]() -> araya::cleanup_action {
		return [weak, sid] {
			if (auto st = weak.lock())
				st->dispose(sid);
		};
	});
	announce(*s);
	return s;
}

std::shared_ptr<session> session_store::prepare(session_id id, create_session_options options) {
	if (id.value.empty())
		throw std::invalid_argument("session id is empty");
	if (store_.contains(id))
		throw std::logic_error("session '" + id.value + "' already exists");

	auto seed = std::move(options.seed);
	for (std::uint64_t i = 0; i < seed.size(); ++i) {
		if (seed[i].seq != i)
			throw std::invalid_argument("seed seq discontinuity at index " + std::to_string(i));
		validate_event(seed[i]);
		seed[i].ignorable = !is_builtin_surface_type(seed[i].type) && !has_projection(seed[i].type);
	}
	auto const seed_size = static_cast<session_seq>(seed.size());

	// Interrupted turns close before the fork cut: closers belong to the
	// inherited history they repair.
	repair_interrupted_turns(seed);

	// The fork cut marker: seeded sessions project their inherited prefix
	// boundary into the log, so a reader of stored history can find it.
	if (options.is_seeded && options.inherited_event_count > 0) {
		seed.push_back(session_event{
			static_cast<session_seq>(seed.size()),
			seed.empty() ? now_ms() : seed.back().time,
			"session/end-seed",
			boost::json::object{{"inherited", options.inherited_event_count}},
			false});
	}

	session_header h;
	h.id = id;
	h.created_at = options.created_at.value_or(now_ms());
	h.cwd = std::move(options.cwd);
	h.parent_session = std::move(options.parent_session);
	h.is_seeded = options.is_seeded;
	h.delegation_depth = options.delegation_depth.value_or(0);
	h.origin = options.origin;
	h.agent_preset = std::move(options.agent_preset);

	// Not make_shared: session's constructor is private, minted only by
	// its friend session_store, so the single-allocation form cannot
	// reach it. shared_ptr(new ...) is the canonical idiom here.
	return std::shared_ptr<session>(new session(
		std::move(h), options.inherited_event_count, seed_size, std::move(seed), projections_, weak_from_this()));
}

void session_store::enter(std::shared_ptr<session> s) {
	if (store_.contains(s->id()))
		throw std::logic_error("session '" + s->id().value + "' already exists");
	store_.emplace(s->id(), std::move(s));
}

void session_store::announce(session const& s) {
	auto it = store_.find(s.id());
	if (it == store_.end())
		throw std::logic_error("announce requires an entered session");
	// Projection cells seed before anyone observes the session: the
	// replay covers the whole log (restores and forks included), so a
	// cell is consistent the moment 'session/created' lands.
	for (auto const& [pid, cell] : projection_cells_)
		cell->seed(s.header(), s.log());
	if (bus_)
		bus_->dispatch(created_key, session_created_msg{it->second});
}

std::shared_ptr<session> session_store::get(session_id const& id) const {
	auto it = store_.find(id);
	return it == store_.end() ? nullptr : it->second;
}

bool session_store::dispose(session_id const& id) {
	auto it = store_.find(id);
	if (it == store_.end())
		return false;
	auto s = std::move(it->second);
	store_.erase(it);
	for (auto const& [pid, cell] : projection_cells_)
		cell->drop(id);
	if (bus_)
		bus_->dispatch(disposed_key, session_disposed_msg{id});
	return true;
}

araya::registration session_store::register_message_projection(plugin_context& caller, message_projection projection) {
	if (has_projection(projection.event_type))
		throw std::logic_error("session message projection '" + projection.event_type + "' is already registered");
	std::string type = projection.event_type;
	projections_.push_back(std::move(projection));
	std::weak_ptr<session_store> weak = weak_from_this();
	return caller.effect([weak, type]() -> araya::cleanup_action {
		return [weak, type] {
			if (auto st = weak.lock()) {
				std::erase_if(st->projections_, [&type](message_projection const& p) { return p.event_type == type; });
			}
		};
	});
}

std::shared_ptr<session> session_store::fork(
	plugin_context& caller,
	session_id const& parent,
	session_id child_id,
	std::optional<session_log_offset> cut) {
	auto it = store_.find(parent);
	if (it == store_.end())
		throw std::logic_error("cannot fork unknown session '" + parent.value + "'");
	auto const& source = it->second;
	auto at = cut.value_or(source->log().size());
	if (at > source->log().size())
		throw std::invalid_argument(
			"fork cut " + std::to_string(at) + " beyond log length " + std::to_string(source->log().size()));

	create_session_options options;
	options.seed = std::vector<session_event>(source->log().begin(), source->log().begin() + at);
	options.inherited_event_count = at;
	options.is_seeded = at > 0;
	options.parent_session = parent;
	options.cwd = source->header().cwd;
	options.origin = source->header().origin;
	options.delegation_depth = source->header().delegation_depth;
	options.agent_preset = source->header().agent_preset;
	return create(caller, child_id.value.empty() ? mint_id() : std::move(child_id), std::move(options));
}

araya::task<void> session_store::flush(session_id id) {
	if (!bus_ || !store_.contains(id))
		co_return;
	co_await bus_->dispatch(flush_key, session_flush_msg{id});
}

bool session_store::has_projection(std::string_view type) const noexcept {
	return std::any_of(projections_.begin(), projections_.end(), [&type](message_projection const& p) {
		return p.event_type == type;
	});
}

void session_store::publish(session& s, session_event& ev) {
	validate_event(ev);
	ev.ignorable = !is_builtin_surface_type(ev.type) && !has_projection(ev.type);
	s.fold(ev, projections_);
	for (auto const& [pid, cell] : projection_cells_)
		cell->drive(s.id(), ev);
	if (bus_ && store_.contains(s.id()))
		bus_->dispatch(appended_key, session_appended_msg{s.id(), ev});
}

namespace {

[[noreturn]] void bad(std::string const& what) { throw std::invalid_argument(what); }

boost::json::object const& message_object(session_event const& ev) {
	auto const* obj = ev.data.if_object();
	if (!obj)
		bad("session event '" + ev.type + "' is not a JSON object");
	if (ev.type == "user/message" || ev.type == "tool/result")
		return *obj;
	auto it = obj->find("message");
	if (it == obj->end() || !it->value().is_object())
		bad("session event '" + ev.type + "' must wrap the message under \"message\"");
	return it->value().as_object();
}

void require_id(boost::json::object const& msg, std::string_view type) {
	auto it = msg.find("id");
	if (it == msg.end() || !it->value().is_string() || it->value().as_string().empty())
		bad("session event '" + std::string(type) + "' message lacks a non-empty string id");
}

void require_content(boost::json::object const& msg, std::string_view type) {
	auto it = msg.find("content");
	if (it == msg.end() || !it->value().is_array())
		bad("session event '" + std::string(type) + "' message lacks an array content");
}

} // namespace

void session_store::validate_event(session_event const& ev) const {
	// Envelope checks for the built-in surface types. Unknown vocabulary
	// is admissible: it rides the log verbatim and the surface skips it.
	if (!is_builtin_surface_type(ev.type))
		return;

	// surface/replace carries a splice, not a message envelope.
	if (ev.type == "surface/replace") {
		auto const* obj = ev.data.if_object();
		if (!obj)
			bad("session event 'surface/replace' is not a JSON object");
		auto sit = obj->find("start_seq");
		auto eit = obj->find("end_seq");
		if (sit == obj->end() || !sit->value().is_int64())
			bad("session event 'surface/replace' lacks an integer start_seq");
		if (eit == obj->end() || !eit->value().is_int64())
			bad("session event 'surface/replace' lacks an integer end_seq");
		auto start = sit->value().as_int64();
		auto end = eit->value().as_int64();
		if (start < 0 || end <= start)
			bad("session event 'surface/replace' span must satisfy 0 <= start_seq < end_seq");
		if (end > static_cast<std::int64_t>(ev.seq))
			bad("session event 'surface/replace' spans into the future (end_seq " + std::to_string(end) +
				" beyond seq " + std::to_string(ev.seq) + ")");
		if (auto mit = obj->find("message"); mit != obj->end()) {
			auto const* msg = mit->value().if_object();
			if (!msg)
				bad("session event 'surface/replace' message is not an object");
			require_id(*msg, ev.type);
			require_content(*msg, ev.type);
			auto rit = msg->find("role");
			if (rit == msg->end() || !rit->value().is_string())
				bad("session event 'surface/replace' message lacks a role");
			auto role = rit->value().as_string();
			if (role != "user" && role != "assistant" && role != "system" && role != "tool_result")
				bad("session event 'surface/replace' message role must be user, assistant, system, or tool_result");
		}
		return;
	}

	auto const& msg = message_object(ev);
	require_id(msg, ev.type);
	require_content(msg, ev.type);

	if (ev.type == "system/message") {
		auto sit = msg.find("source");
		if (sit == msg.end() || !sit->value().is_object())
			bad("session event 'system/message' lacks a source object");
		auto const& src = sit->value().as_object();
		auto pit = src.find("kind");
		if (pit == src.end() || pit->value() != "plugin")
			bad("session event 'system/message' source kind must be "
				"\"plugin\"");
		auto nit = src.find("plugin");
		if (nit == src.end() || !nit->value().is_string() || nit->value().as_string().empty())
			bad("session event 'system/message' source lacks a non-empty "
				"plugin name");
	} else if (ev.type == "tool/result") {
		auto const& content = msg.at("content").as_array();
		if (content.size() != 1)
			bad("session event 'tool/result' content must have exactly "
				"one block");
		auto const* block = content[0].if_object();
		if (!block)
			bad("session event 'tool/result' content block is not an "
				"object");
		auto tit = block->find("tool_call_id");
		if (tit == block->end() || !tit->value().is_string() || tit->value().as_string().empty())
			bad("session event 'tool/result' block lacks a non-empty "
				"tool_call_id");
		std::string_view call_id = tit->value().as_string();
		auto sit = msg.find("source");
		if (sit == msg.end() || !sit->value().is_object())
			bad("session event 'tool/result' lacks a source object");
		auto const& src = sit->value().as_object();
		auto cit = src.find("call_id");
		if (cit == src.end() || !cit->value().is_string() || cit->value().as_string() != call_id)
			bad("session event 'tool/result' source call_id does not "
				"match the block tool_call_id");
	}
}

} // namespace araya::session
