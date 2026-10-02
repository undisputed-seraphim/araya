#include "araya/agent-loop/inbox.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace araya::agent {

namespace {

char const* target_name(inbox_target target) { return target == inbox_target::next_turn ? "next-turn" : "next-step"; }

bool as_index(boost::json::value const& node, std::int64_t& out) {
	if (node.is_int64()) {
		out = node.as_int64();
		return true;
	}
	if (node.is_uint64()) {
		auto const value = node.as_uint64();
		if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
			return false;
		out = static_cast<std::int64_t>(value);
		return true;
	}
	return false;
}

std::vector<boost::json::value>* queue_for(inbox_state& state, std::string_view target) {
	if (target == "next-turn")
		return &state.next_turn;
	if (target == "next-step")
		return &state.next_step;
	return nullptr;
}

} // namespace

boost::json::value inbox_splice_data(
	inbox_target target,
	std::int64_t start,
	std::int64_t removed_count,
	boost::json::array inserted,
	std::string_view outcome) {
	boost::json::object data;
	data["target"] = target_name(target);
	data["start"] = start;
	if (removed_count != 0)
		data["removedCount"] = removed_count;
	data["inserted"] = std::move(inserted);
	if (!outcome.empty())
		data["outcome"] = std::string(outcome);
	return data;
}

boost::json::value inbox_append_data(inbox_target target, boost::json::value const& message) {
	// An index past the end clamps to the end (toSpliced), so append needs
	// no queue length.
	return inbox_splice_data(target, std::numeric_limits<std::int64_t>::max(), 0, boost::json::array{message});
}

boost::json::value inbox_prepend_data(inbox_target target, boost::json::value const& message) {
	return inbox_splice_data(target, 0, 0, boost::json::array{message});
}

boost::json::value inbox_claim_data(inbox_target target, std::int64_t count) {
	return inbox_splice_data(target, 0, count);
}

std::string_view inbox_message_id_view(boost::json::value const& message) {
	auto const* object = message.if_object();
	if (!object)
		return {};
	auto it = object->find("id");
	if (it == object->end() || !it->value().is_string())
		return {};
	return it->value().as_string();
}

std::string inbox_message_id(boost::json::value const& message) {
	auto const id = inbox_message_id_view(message);
	return id.empty() ? std::string{} : std::string(id);
}

inbox_mutation make_inbox_splice(
	inbox_state const& state,
	inbox_target target,
	std::int64_t start,
	std::int64_t delete_count,
	boost::json::array inserted,
	bool discard) {
	auto const& queue = target == inbox_target::next_turn ? state.next_turn : state.next_step;
	auto const length = static_cast<std::int64_t>(queue.size());
	if (start < 0)
		start = std::max(length + start, std::int64_t{0});
	else
		start = std::min(start, length);
	if (delete_count < 0)
		delete_count = 0;
	delete_count = std::min(delete_count, length - start);
	if (delete_count == 0 && inserted.empty()) {
		inbox_mutation none;
		none.noop = true;
		return none;
	}

	inbox_mutation result;
	result.removed.assign(queue.begin() + start, queue.begin() + start + delete_count);
	result.data = inbox_splice_data(
		target, start, delete_count, std::move(inserted), (discard && delete_count > 0) ? "canceled" : "");
	return result;
}

void apply_inbox_splice(inbox_state& state, boost::json::value const& data) {
	auto const* object = data.if_object();
	if (!object)
		return;

	auto target_it = object->find("target");
	if (target_it == object->end() || !target_it->value().is_string())
		return;
	auto* queue = queue_for(state, target_it->value().as_string());
	if (!queue)
		return;

	auto start_it = object->find("start");
	std::int64_t start = 0;
	if (start_it == object->end() || !as_index(start_it->value(), start))
		return;

	std::int64_t removed = 0;
	if (auto it = object->find("removedCount"); it != object->end() && !as_index(it->value(), removed))
		return;

	// toSpliced coordinates: clamp start (negative counts from the end) and
	// the removal count into range.
	auto const length = static_cast<std::int64_t>(queue->size());
	if (start < 0)
		start = std::max(length + start, std::int64_t{0});
	else
		start = std::min(start, length);
	if (removed < 0)
		removed = 0;
	removed = std::min(removed, length - start);

	queue->erase(queue->begin() + start, queue->begin() + start + removed);

	auto inserted_it = object->find("inserted");
	if (inserted_it == object->end() || !inserted_it->value().is_array())
		return;
	auto const& inserted = inserted_it->value().as_array();
	if (inserted.empty())
		return;

	// Insertions are checked against the ids still present in either queue
	// (and against earlier insertions) so the cross-queue uniqueness
	// invariant survives a misbehaving producer. The set is built only when
	// there is something to insert: claim/remove events skip it entirely.
	std::unordered_set<std::string> ids;
	ids.reserve(state.next_turn.size() + state.next_step.size() + inserted.size());
	for (auto const& message : state.next_turn) {
		auto id = inbox_message_id(message);
		if (!id.empty())
			ids.insert(std::move(id));
	}
	for (auto const& message : state.next_step) {
		auto id = inbox_message_id(message);
		if (!id.empty())
			ids.insert(std::move(id));
	}

	auto at = queue->begin() + start;
	for (auto const& message : inserted) {
		auto id = inbox_message_id(message);
		if (id.empty() || ids.contains(id))
			continue;
		at = queue->insert(at, message);
		++at;
		ids.insert(std::move(id));
	}
}

araya::session::event_projection<inbox_state> inbox_projection() {
	return araya::session::event_projection<inbox_state>{
		.name = "agent-inbox",
		.types = {std::string(k_inbox_spliced_event)},
		.init = [](araya::session::session_header const&) { return inbox_state{}; },
		.apply = [](inbox_state& state,
					araya::session::session_event const& event) { apply_inbox_splice(state, event.data); }};
}

} // namespace araya::agent
