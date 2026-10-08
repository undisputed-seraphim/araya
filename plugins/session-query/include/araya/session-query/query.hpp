#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"
#include "araya/session/session_types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Session query/search: resolve the logical session set (live store +
// durable persistence), fold each log into searchable event documents, and
// answer structured + full-text queries over them. The service is
// backend-shaped: the exact reads, filtering, extraction, and lineage
// tracing are backend-independent, while the two ranked search entry points
// are the only part a different backend (an FTS index) replaces.
//
// The shipped `session-query` engine is fully in-memory: it scans the
// resolved documents and applies the harness's literal, case-insensitive,
// whitespace-flexible text match. A later `session-query-sqlite` backend
// provides the same key and swaps in ranked FTS5 search.
namespace araya::session_query {

using session::session_event;
using session::session_header;
using session::session_id;
using session::session_seq;

// Closed error taxonomy; every engine failure carries one of these so the
// tool boundary can map it to a model-safe message.
enum class query_error_code {
	invalid_config,
	invalid_query,
	invalid_filter,
	invalid_cursor,
	stale_cursor,
	session_not_found,
	invalid_surface,
	source_conflict,
	index_failed,
	aborted,
};

class query_error : public std::runtime_error {
public:
	query_error(query_error_code code, std::string message)
		: std::runtime_error(std::move(message))
		, code_(code) {}

	query_error_code code() const noexcept { return code_; }

private:
	query_error_code code_;
};

enum class availability : std::uint8_t { live, persisted };

// Where an event sits relative to the folded surface: `current` nodes are in
// the LLM-facing history, `shadowed` events were replaced by a
// `surface/replace`, and everything else is `log_only`.
enum class event_surface : std::uint8_t { current, shadowed, log_only };

// One logical session and its observation sources.
struct session_record {
	session_header header;
	bool live = false;
	bool persisted = false;
};

// A full observation of one logical session: header plus canonical log
// (seq 0..n-1). Live sessions read the store; persisted ones the backend.
struct session_source {
	session_header header;
	std::vector<session_event> events;
	bool live = false;
	bool persisted = false;
};

// Filter dimensions. An empty filter vector matches everything; filters are
// ANDed and a clause's `values` are ORed (the harness's rule).
struct session_filter {
	enum class kind : std::uint8_t { id, cwd, created_at, parent, availability };

	kind clause = kind::id;
	std::vector<std::string> values; // id | cwd | parent id | "live"/"persisted"
	// parent clause only: also match sessions with no parent (root marker).
	bool allow_root = false;
	std::optional<std::int64_t> from; // created_at lower bound (inclusive ms)
	std::optional<std::int64_t> to;	  // created_at upper bound (inclusive ms)
};

struct event_filter {
	enum class kind : std::uint8_t { seq, time, type, surface, text };

	kind clause = kind::seq;
	std::vector<std::string> values; // type | "current"/"shadowed"/"log-only"
	std::optional<std::int64_t> from;
	std::optional<std::int64_t> to;
	std::string text; // text clause: the literal scan term
};

struct event_record {
	session_id session;
	session_seq seq = 0;
	std::string type;
	std::int64_t time = 0;
	event_surface surface = event_surface::log_only;
};

struct event_search_hit : event_record {
	std::string snippet;
};

struct session_search_hit {
	session_record session;
	event_search_hit best_match;
};

struct session_search_request {
	std::string query;
	std::vector<session_filter> session_filters;
	std::vector<event_filter> event_filters;
	std::size_t limit = 20;
	std::string cursor;
};

struct event_search_request {
	session_id session;
	std::string query;
	std::vector<event_filter> filters;
	std::size_t limit = 20;
	std::string cursor;
};

struct session_search_page {
	std::vector<session_search_hit> items;
	std::string next_cursor;
};

struct event_search_page {
	session_header session;
	std::vector<event_search_hit> items;
	std::string next_cursor;
};

// An exact read of one event with up to N neighbours on each side.
struct event_read {
	session_event target;
	std::vector<session_event> before;
	std::vector<session_event> after;
};

struct lineage_node {
	session_header header;
	bool live = false;
	bool persisted = false;
};

struct descendant_node {
	lineage_node node;
	std::uint32_t depth = 1;
};

struct session_trace {
	lineage_node target;
	std::vector<lineage_node> ancestors; // nearest first
	std::vector<descendant_node> descendants;
};

struct event_trace {
	event_record target;
	std::optional<event_record> replaced_by;	 // the replace that shadowed target
	std::vector<event_record> replacement_chain; // target -> ... -> newest replace
	std::vector<event_record> replaced_events;	 // if target is a replace, what it removed
	std::vector<event_record> source_events;	 // events cited as sources (none today)
	std::vector<event_record> derived_events;	 // events citing target as a source
};

// -- backend-independent helpers (shared with a future index backend) -------

// The semantic text of one event, or "" when it contributes nothing (the
// same extraction the search index ranks over).
std::string extract_event_text(session_event const& event);

// One surface classification per event, positionally aligned with `events`.
std::vector<event_surface> classify_surfaces(std::vector<session_event> const& events);

bool session_matches(
	std::vector<session_filter> const& filters,
	session_header const& header,
	bool live,
	bool persisted);

bool event_matches(std::vector<event_filter> const& filters, event_record const& record, std::string_view text);

// -- the service -----------------------------------------------------------

class session_query_service {
public:
	virtual ~session_query_service() = default;

	// The resolved logical session set, newest first.
	virtual std::vector<session_record> list_sessions() const = 0;

	// A full observation of one logical session, or nullopt when unknown.
	virtual std::optional<session_source> read_session(session_id const& id) const = 0;

	// The sessions passing the structured filters, newest first.
	virtual std::vector<session_record> filter_sessions(std::vector<session_filter> const& filters) const = 0;

	// Ranked session hits: one best-matching event per session.
	virtual session_search_page search_sessions(session_search_request const& request) const = 0;

	// Ranked event hits within one session.
	virtual event_search_page search_events(event_search_request const& request) const = 0;

	// The events of one session passing the filters (literal text scan for a
	// text clause), in log order.
	virtual std::vector<event_search_hit>
	filter_events(session_id const& session, std::vector<event_filter> const& filters) const = 0;

	// The event at `seq` with up to `before`/`after` neighbours.
	virtual std::optional<event_read>
	read_event(session_id const& session, session_seq seq, unsigned before, unsigned after) const = 0;

	// The first user-message text as a title, or nullopt.
	virtual std::optional<std::string> read_title(session_id const& id) const = 0;

	virtual std::optional<session_trace> trace_session(session_id const& id) const = 0;

	virtual std::optional<event_trace> trace_event(session_id const& session, session_seq seq) const = 0;
};

inline constexpr araya::service_key<session_query_service> session_query_key{"session-query", 1};

// The plugin descriptor: apply() resolves `sessions` (required) and
// `session.persistence` (optional) and provides `session-query`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::session_query
