#include "araya/session-query/query.hpp"

#include "araya/config.hpp"
#include "araya/persistence/persistence.hpp"
#include "araya/session/store.hpp"
#include "araya/session/surface.hpp"
#include "araya/util/json.hpp"
#include "araya/util/string.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::session_query {
namespace {

using araya::persistence::persistence_key;
using araya::persistence::session_persistence;
using araya::session::fold_event;
using araya::session::message_projection;
using araya::session::session_message;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::session::surface_plan;

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::vector<std::string> split_words(std::string_view text) {
	std::vector<std::string> words;
	std::size_t i = 0;
	while (i < text.size()) {
		while (i < text.size() && is_space(text[i]))
			++i;
		std::size_t start = i;
		while (i < text.size() && !is_space(text[i]))
			++i;
		if (i > start)
			words.emplace_back(text.substr(start, i - start));
	}
	return words;
}

// A literal, case-insensitive, whitespace-flexible match of a word sequence
// against text. Returns the number of non-overlapping matches and the first
// match's byte span (for the snippet).
struct match_info {
	std::size_t count = 0;
	std::size_t first = std::string_view::npos;
	std::size_t length = 0;
};

bool word_at(std::string_view text, std::size_t pos, std::string_view word) {
	if (pos + word.size() > text.size())
		return false;
	for (std::size_t i = 0; i < word.size(); ++i) {
		if (lower(text[pos + i]) != lower(word[i]))
			return false;
	}
	return true;
}

match_info find_matches(std::string_view text, std::vector<std::string> const& words) {
	match_info info;
	if (words.empty() || text.empty())
		return info;
	std::size_t i = 0;
	while (i < text.size()) {
		std::size_t p = i;
		bool ok = true;
		for (std::size_t w = 0; w < words.size(); ++w) {
			if (!word_at(text, p, words[w])) {
				ok = false;
				break;
			}
			p += words[w].size();
			if (w + 1 < words.size()) {
				if (p >= text.size() || !is_space(text[p])) {
					ok = false;
					break;
				}
				while (p < text.size() && is_space(text[p]))
					++p;
			}
		}
		if (ok) {
			if (info.count == 0) {
				info.first = i;
				info.length = p - i;
			}
			++info.count;
			i = p;
		} else {
			++i;
		}
	}
	return info;
}

std::string make_snippet(std::string_view text, match_info const& match, std::size_t max_chars = 160) {
	if (text.empty())
		return {};
	if (match.first == std::string_view::npos)
		return std::string(text.substr(0, std::min(max_chars, text.size())));
	std::size_t const lead = max_chars / 3;
	std::size_t const start = match.first > lead ? match.first - lead : 0;
	std::size_t const end = std::min(text.size(), start + max_chars);
	std::string out;
	if (start > 0)
		out += "...";
	out += text.substr(start, end - start);
	if (end < text.size())
		out += "...";
	return out;
}

constexpr char const* surface_name(event_surface surface) {
	switch (surface) {
	case event_surface::current:
		return "current";
	case event_surface::shadowed:
		return "shadowed";
	case event_surface::log_only:
		return "log-only";
	}
	return "log-only";
}

// -- event text extraction --------------------------------------------------

void append_text_value(std::string& out, boost::json::value const& value) {
	if (value.is_string()) {
		auto const text = value.as_string();
		if (!text.empty()) {
			if (!out.empty())
				out += '\n';
			out += text;
		}
		return;
	}
	auto const* array = value.if_array();
	if (!array)
		return;
	for (auto const& block : *array) {
		auto const* object = block.if_object();
		if (!object) {
			append_text_value(out, block);
			continue;
		}
		auto const type = araya::util::json::get_string(*object, "type");
		if (type == "text") {
			auto const text = araya::util::json::get_string(*object, "text");
			if (!text.empty()) {
				if (!out.empty())
					out += '\n';
				out += text;
			}
		} else if (type == "tool_call") {
			auto const name = araya::util::json::get_string(*object, "name");
			auto const arguments = araya::util::json::get_string(*object, "arguments");
			if (!name.empty() || !arguments.empty()) {
				if (!out.empty())
					out += '\n';
				out += name;
				if (!arguments.empty()) {
					out += ' ';
					out += arguments;
				}
			}
		} else if (type == "tool_result") {
			if (auto const* content = object->if_contains("content"))
				append_text_value(out, *content);
		}
		// reasoning and image blocks are intentionally not searchable.
	}
}

// The content field of an event's message object.
boost::json::value const* content_of(boost::json::object const& object, bool wrapped) {
	if (!wrapped)
		return object.if_contains("content");
	auto const* message = araya::util::json::get_object(object, "message");
	return message ? message->if_contains("content") : nullptr;
}

// -- lineage helpers --------------------------------------------------------

lineage_node to_node(session_header const& header, bool live, bool persisted) {
	return lineage_node{header, live, persisted};
}

} // namespace

std::string extract_event_text(session_event const& event) {
	std::string text;
	auto const* object = event.data.if_object();
	if (!object)
		return text;
	if (event.type == "user/message") {
		if (auto const* content = content_of(*object, false))
			append_text_value(text, *content);
	} else if (event.type == "assistant/message" || event.type == "system/message") {
		if (auto const* content = content_of(*object, true))
			append_text_value(text, *content);
	} else if (event.type == "tool/result") {
		if (auto const* content = content_of(*object, false))
			append_text_value(text, *content);
	} else if (event.type == "turn/end") {
		auto const reason = araya::util::json::get_string(*object, "reason");
		if (reason != "completed" && reason != "max_tokens")
			text = reason;
	}
	return text;
}

std::vector<event_surface> classify_surfaces(std::vector<session_event> const& events) {
	araya::session::session_surface surface;
	std::set<session_seq> shadowed;
	for (auto const& ev : events) {
		auto plan = fold_event(ev, {});
		if (plan.action == surface_plan::kind::append) {
			if (plan.message)
				surface.push(ev.seq, *plan.message);
		} else if (plan.action == surface_plan::kind::replace) {
			for (session_seq seq = plan.start; seq < plan.end; ++seq)
				shadowed.insert(seq);
			surface.replace(plan.start, plan.end, ev.seq, plan.message);
		}
	}
	std::set<session_seq> current(surface.source_seqs().begin(), surface.source_seqs().end());
	std::vector<event_surface> out;
	out.reserve(events.size());
	for (auto const& ev : events) {
		if (current.contains(ev.seq))
			out.push_back(event_surface::current);
		else if (shadowed.contains(ev.seq))
			out.push_back(event_surface::shadowed);
		else
			out.push_back(event_surface::log_only);
	}
	return out;
}

bool session_matches(
	std::vector<session_filter> const& filters,
	session_header const& header,
	bool live,
	bool persisted) {
	for (auto const& filter : filters) {
		switch (filter.clause) {
		case session_filter::kind::id:
			if (std::find(filter.values.begin(), filter.values.end(), header.id.value) == filter.values.end())
				return false;
			break;
		case session_filter::kind::cwd:
			if (!header.cwd ||
				std::find(filter.values.begin(), filter.values.end(), *header.cwd) == filter.values.end())
				return false;
			break;
		case session_filter::kind::created_at:
			if (filter.from && header.created_at < *filter.from)
				return false;
			if (filter.to && header.created_at > *filter.to)
				return false;
			break;
		case session_filter::kind::parent:
			if (!header.parent_session) {
				if (!filter.allow_root)
					return false;
			} else if (
				std::find(filter.values.begin(), filter.values.end(), header.parent_session->value) ==
				filter.values.end()) {
				return false;
			}
			break;
		case session_filter::kind::availability: {
			bool ok = false;
			for (auto const& value : filter.values) {
				if (value == "live" && live)
					ok = true;
				if (value == "persisted" && persisted)
					ok = true;
			}
			if (!ok)
				return false;
			break;
		}
		}
	}
	return true;
}

bool event_matches(std::vector<event_filter> const& filters, event_record const& record, std::string_view text) {
	for (auto const& filter : filters) {
		switch (filter.clause) {
		case event_filter::kind::seq:
			if (filter.from && static_cast<std::int64_t>(record.seq) < *filter.from)
				return false;
			if (filter.to && static_cast<std::int64_t>(record.seq) > *filter.to)
				return false;
			break;
		case event_filter::kind::time:
			if (filter.from && record.time < *filter.from)
				return false;
			if (filter.to && record.time > *filter.to)
				return false;
			break;
		case event_filter::kind::type:
			if (std::find(filter.values.begin(), filter.values.end(), record.type) == filter.values.end())
				return false;
			break;
		case event_filter::kind::surface:
			if (std::find(filter.values.begin(), filter.values.end(), surface_name(record.surface)) ==
				filter.values.end())
				return false;
			break;
		case event_filter::kind::text:
			if (find_matches(text, split_words(filter.text)).count == 0)
				return false;
			break;
		}
	}
	return true;
}

namespace {

// -- the in-memory engine ---------------------------------------------------

struct ranked_hit {
	event_search_hit hit;
	std::size_t count = 0;
	std::size_t text_length = 0;
	std::string text;
};

// Best event first: match count desc, text length asc, time desc, seq desc.
bool better_event(ranked_hit const& a, ranked_hit const& b) {
	if (a.count != b.count)
		return a.count > b.count;
	if (a.text_length != b.text_length)
		return a.text_length < b.text_length;
	if (a.hit.time != b.hit.time)
		return a.hit.time > b.hit.time;
	return a.hit.seq > b.hit.seq;
}

bool better_session(session_search_hit const& a, session_search_hit const& b) {
	if (a.best_match.seq != b.best_match.seq)
		return a.best_match.seq > b.best_match.seq;
	return a.session.header.id.value < b.session.header.id.value;
}

class in_memory_engine : public session_query_service {
public:
	std::shared_ptr<session_store> store;
	std::shared_ptr<session_persistence> persistence;
	unsigned read_window_max = 50;

	std::vector<session_record> list_sessions() const override {
		std::map<std::string, session_record> by_id;
		if (persistence) {
			for (auto const& id : persistence->list()) {
				try {
					auto stored = persistence->read(id);
					if (stored)
						by_id[stored->header.id.value] = session_record{stored->header, false, true};
				} catch (...) {
					// An unreadable stored session is skipped, not fatal.
				}
			}
		}
		if (store) {
			for (auto const& id : store->list()) {
				auto session = store->get(id);
				if (!session)
					continue;
				auto& record = by_id[id.value];
				record.header = session->header();
				record.live = true;
			}
		}
		std::vector<session_record> records;
		records.reserve(by_id.size());
		for (auto const& [id, record] : by_id)
			records.push_back(record);
		std::sort(records.begin(), records.end(), [](session_record const& a, session_record const& b) {
			if (a.header.created_at != b.header.created_at)
				return a.header.created_at > b.header.created_at;
			return a.header.id.value < b.header.id.value;
		});
		return records;
	}

	std::optional<session_source> read_session(session_id const& id) const override {
		if (store) {
			if (auto session = store->get(id)) {
				bool persisted = false;
				if (persistence) {
					try {
						persisted = persistence->read(id).has_value();
					} catch (...) {
					}
				}
				return session_source{session->header(), session->log(), true, persisted};
			}
		}
		if (persistence) {
			try {
				if (auto stored = persistence->read(id))
					return session_source{stored->header, stored->events, false, true};
			} catch (...) {
			}
		}
		return std::nullopt;
	}

	std::vector<session_record> filter_sessions(std::vector<session_filter> const& filters) const override {
		std::vector<session_record> out;
		for (auto& record : list_sessions()) {
			if (session_matches(filters, record.header, record.live, record.persisted))
				out.push_back(std::move(record));
		}
		return out;
	}

	session_search_page search_sessions(session_search_request const& request) const override {
		auto const words = split_words(request.query);
		if (words.empty())
			throw query_error(query_error_code::invalid_query, "query must not be empty");
		std::vector<session_search_hit> hits;
		for (auto const& record : filter_sessions(request.session_filters)) {
			auto source = read_session(record.header.id);
			if (!source)
				continue;
			auto const ranked = rank_events(*source, request.event_filters, words);
			if (ranked.empty())
				continue;
			hits.push_back(session_search_hit{record, ranked.front().hit});
		}
		std::sort(hits.begin(), hits.end(), better_session);
		std::size_t const offset = decode_cursor(request.cursor);
		std::size_t const limit = request.limit == 0 ? 20 : request.limit;
		session_search_page page;
		for (std::size_t i = offset; i < hits.size() && page.items.size() < limit; ++i)
			page.items.push_back(std::move(hits[i]));
		if (offset + page.items.size() < hits.size())
			page.next_cursor = encode_cursor(offset + page.items.size());
		return page;
	}

	event_search_page search_events(event_search_request const& request) const override {
		auto const words = split_words(request.query);
		if (words.empty())
			throw query_error(query_error_code::invalid_query, "query must not be empty");
		auto source = read_session(request.session);
		if (!source)
			throw query_error(query_error_code::session_not_found, "session not found: " + request.session.value);
		auto ranked = rank_events(*source, request.filters, words);
		std::size_t const offset = decode_cursor(request.cursor);
		std::size_t const limit = request.limit == 0 ? 20 : request.limit;
		event_search_page page;
		page.session = source->header;
		for (std::size_t i = offset; i < ranked.size() && page.items.size() < limit; ++i)
			page.items.push_back(std::move(ranked[i].hit));
		if (offset + page.items.size() < ranked.size())
			page.next_cursor = encode_cursor(offset + page.items.size());
		return page;
	}

	std::vector<event_search_hit>
	filter_events(session_id const& session, std::vector<event_filter> const& filters) const override {
		auto source = read_session(session);
		std::vector<event_search_hit> hits;
		if (!source)
			return hits;
		auto const surfaces = classify_surfaces(source->events);
		for (std::size_t i = 0; i < source->events.size(); ++i) {
			auto text = extract_event_text(source->events[i]);
			event_record record{
				session, source->events[i].seq, source->events[i].type, source->events[i].time, surfaces[i]};
			if (!event_matches(filters, record, text))
				continue;
			event_search_hit hit{record, {}};
			for (auto const& filter : filters) {
				if (filter.clause == event_filter::kind::text) {
					auto const match = find_matches(text, split_words(filter.text));
					hit.snippet = make_snippet(text, match);
					break;
				}
			}
			hits.push_back(std::move(hit));
		}
		return hits;
	}

	std::optional<event_read>
	read_event(session_id const& session, session_seq seq, unsigned before, unsigned after) const override {
		auto source = read_session(session);
		if (!source)
			return std::nullopt;
		auto const& events = source->events;
		std::size_t index = 0;
		for (; index < events.size(); ++index) {
			if (events[index].seq == seq)
				break;
		}
		if (index == events.size())
			return std::nullopt;
		unsigned const back = std::min<unsigned>(before, static_cast<unsigned>(index));
		std::size_t const end = std::min(events.size(), index + 1 + static_cast<std::size_t>(after));
		event_read read;
		read.target = events[index];
		for (std::size_t i = index - back; i < index; ++i)
			read.before.push_back(events[i]);
		for (std::size_t i = index + 1; i < end; ++i)
			read.after.push_back(events[i]);
		return read;
	}

	std::optional<std::string> read_title(session_id const& id) const override {
		auto source = read_session(id);
		if (!source)
			return std::nullopt;
		for (auto const& ev : source->events) {
			if (ev.type != "user/message")
				continue;
			auto text = araya::util::string::trim(extract_event_text(ev));
			if (text.empty())
				continue;
			if (text.size() > 80)
				text.resize(80);
			return text;
		}
		return std::nullopt;
	}

	std::optional<session_trace> trace_session(session_id const& id) const override {
		auto const records = list_sessions();
		std::map<std::string, session_record const*> by_id;
		for (auto const& record : records)
			by_id[record.header.id.value] = &record;
		auto target = by_id.find(id.value);
		if (target == by_id.end())
			return std::nullopt;

		session_trace trace;
		trace.target = to_node(target->second->header, target->second->live, target->second->persisted);

		std::set<std::string> seen;
		seen.insert(id.value);
		auto parent = target->second->header.parent_session;
		while (parent && !seen.contains(parent->value)) {
			auto it = by_id.find(parent->value);
			if (it == by_id.end())
				break;
			seen.insert(parent->value);
			trace.ancestors.push_back(to_node(it->second->header, it->second->live, it->second->persisted));
			parent = it->second->header.parent_session;
		}

		// Breadth-first descendants over the parent links.
		std::map<std::string, std::vector<std::string>> children;
		for (auto const& record : records) {
			if (record.header.parent_session)
				children[record.header.parent_session->value].push_back(record.header.id.value);
		}
		std::vector<std::pair<std::string, std::uint32_t>> frontier{{id.value, 0}};
		while (!frontier.empty()) {
			auto const [current, depth] = frontier.back();
			frontier.pop_back();
			for (auto const& child : children[current]) {
				auto it = by_id.find(child);
				if (it == by_id.end())
					continue;
				trace.descendants.push_back(
					descendant_node{to_node(it->second->header, it->second->live, it->second->persisted), depth + 1});
				frontier.emplace_back(child, depth + 1);
			}
		}
		return trace;
	}

	std::optional<event_trace> trace_event(session_id const& session, session_seq seq) const override {
		auto source = read_session(session);
		if (!source)
			return std::nullopt;
		auto const& events = source->events;
		auto const surfaces = classify_surfaces(events);
		std::optional<std::size_t> index;
		for (std::size_t i = 0; i < events.size(); ++i) {
			if (events[i].seq == seq) {
				index = i;
				break;
			}
		}
		if (!index)
			return std::nullopt;
		auto record_at = [&](std::size_t i) {
			return event_record{session, events[i].seq, events[i].type, events[i].time, surfaces[i]};
		};

		event_trace trace;
		trace.target = record_at(*index);
		for (std::size_t i = 0; i < events.size(); ++i) {
			if (events[i].type != "surface/replace")
				continue;
			auto const* object = events[i].data.if_object();
			if (!object)
				continue;
			auto const start = araya::util::json::opt_int(*object, "start_seq");
			auto const end = araya::util::json::opt_int(*object, "end_seq");
			if (!start || !end)
				continue;
			if (events[i].seq == seq) {
				for (std::size_t j = 0; j < events.size(); ++j) {
					auto const s = static_cast<std::int64_t>(events[j].seq);
					if (s >= *start && s < *end)
						trace.replaced_events.push_back(record_at(j));
				}
			}
			auto const s = static_cast<std::int64_t>(seq);
			if (s >= *start && s < *end) {
				trace.replaced_by = record_at(i);
				trace.replacement_chain.push_back(record_at(i));
			}
		}
		return trace;
	}

private:
	std::vector<ranked_hit> rank_events(
		session_source const& source,
		std::vector<event_filter> const& filters,
		std::vector<std::string> const& words) const {
		std::vector<ranked_hit> ranked;
		auto const surfaces = classify_surfaces(source.events);
		for (std::size_t i = 0; i < source.events.size(); ++i) {
			auto text = extract_event_text(source.events[i]);
			event_record record{
				source.header.id, source.events[i].seq, source.events[i].type, source.events[i].time, surfaces[i]};
			if (!event_matches(filters, record, text))
				continue;
			auto const match = find_matches(text, words);
			if (match.count == 0)
				continue;
			event_search_hit hit{record, make_snippet(text, match)};
			ranked.push_back(ranked_hit{std::move(hit), match.count, text.size(), std::move(text)});
		}
		std::sort(ranked.begin(), ranked.end(), better_event);
		return ranked;
	}

	static std::string encode_cursor(std::size_t offset) { return "v1:" + std::to_string(offset); }

	static std::size_t decode_cursor(std::string const& cursor) {
		if (cursor.empty())
			return 0;
		constexpr std::string_view prefix = "v1:";
		if (cursor.compare(0, prefix.size(), prefix) != 0)
			throw query_error(query_error_code::invalid_cursor, "invalid cursor");
		std::string const digits = cursor.substr(prefix.size());
		if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
			throw query_error(query_error_code::invalid_cursor, "invalid cursor");
		return static_cast<std::size_t>(std::stoull(digits));
	}
};

struct session_query_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto engine = std::make_shared<in_memory_engine>();
		engine->store = ctx.require<session_store>(sessions_key).shared();
		if (auto lease = ctx.find<session_persistence>(persistence_key))
			engine->persistence = lease->shared();
		ctx.provide(session_query_key, std::shared_ptr<session_query_service>(std::move(engine)));
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_session_query(araya::plugin_config const&) {
	return std::make_unique<session_query_plugin>();
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"session.persistence", 1}, false, {}},
};
static const araya::provision_spec g_provs[]{{araya::service_id{"session-query", 1}}};
static const araya::plugin_descriptor g_descriptor{"session-query", g_deps, g_provs, &make_session_query};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::session_query
