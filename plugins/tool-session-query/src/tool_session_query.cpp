#include "araya/tool-session-query/tool_session_query.hpp"

#include "araya/agent-loop/agent.hpp"
#include "araya/config.hpp"
#include "araya/session-query/query.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"
#include "araya/util/string.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::tool_session_query {
namespace {

using araya::session::session_header;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::session_query::availability;
using araya::session_query::descendant_node;
using araya::session_query::event_filter;
using araya::session_query::event_search_hit;
using araya::session_query::event_search_request;
using araya::session_query::lineage_node;
using araya::session_query::query_error;
using araya::session_query::session_filter;
using araya::session_query::session_query_key;
using araya::session_query::session_query_service;
using araya::session_query::session_search_request;
using araya::tools::error_result;
using araya::tools::text_result;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;

constexpr araya::config_key<std::uint64_t> max_results_key{"maxSearchResults"};

constexpr araya::config_field g_config[] = {
	field(max_results_key, "Maximum number of search results returned.", "100"),
};

constexpr std::string_view k_prompt =
	"Use session_search to find relevant work from prior sessions, or session_event_search to search earlier events "
	"in one session. Search results are workspace-scoped. Follow a useful hit with session_trace, "
	"session_event_trace, or session_event_read when you need lineage, relationships, or exact data.";

// -- argument helpers -------------------------------------------------------

std::vector<std::string> string_array(boost::json::object const& object, std::string_view key) {
	std::vector<std::string> out;
	auto const* array = araya::util::json::get_array(object, key);
	if (!array)
		return out;
	for (auto const& entry : *array) {
		if (entry.is_string())
			out.emplace_back(entry.as_string());
	}
	return out;
}

std::optional<std::string> opt_string(boost::json::object const& object, std::string_view key) {
	auto it = object.find(key);
	if (it == object.end() || !it->value().is_string())
		return std::nullopt;
	return std::string(it->value().as_string());
}

std::optional<std::uint64_t> opt_uint(boost::json::object const& object, std::string_view key) {
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	return araya::util::json::opt_uint(it->value());
}

bool bool_field(boost::json::object const& object, std::string_view key) {
	auto it = object.find(key);
	return it != object.end() && araya::util::json::get_bool(it->value());
}

std::int64_t parse_timestamp(std::string const& text) {
	auto const trimmed = araya::util::string::trim(text);
	if (trimmed.empty())
		throw std::runtime_error("timestamp must not be empty");
	// Plain epoch milliseconds.
	bool digits = true;
	std::size_t start = (!trimmed.empty() && trimmed[0] == '-') ? 1 : 0;
	if (start < trimmed.size()) {
		for (std::size_t i = start; i < trimmed.size(); ++i) {
			if (trimmed[i] < '0' || trimmed[i] > '9') {
				digits = false;
				break;
			}
		}
	} else {
		digits = false;
	}
	if (digits)
		return std::stoll(trimmed);
	// ISO 8601 UTC: YYYY-MM-DDTHH:MM:SS[.fff]Z.
	std::tm tm{};
	std::istringstream stream(trimmed);
	stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
	if (stream.fail())
		throw std::runtime_error("timestamp must be epoch milliseconds or ISO 8601 (e.g. 2026-01-01T00:00:00Z)");
	std::int64_t millis = 0;
	if (stream.peek() == '.') {
		stream.get();
		std::string fraction;
		while (std::isdigit(stream.peek()))
			fraction.push_back(static_cast<char>(stream.get()));
		fraction.resize(3, '0');
		millis = std::stoll(fraction.substr(0, 3));
	}
#if defined(_WIN32)
	auto const seconds = _mkgmtime(&tm);
#else
	auto const seconds = timegm(&tm);
#endif
	return static_cast<std::int64_t>(seconds) * 1000 + millis;
}

std::string format_time(std::int64_t millis) {
	std::time_t const seconds = static_cast<std::time_t>(millis / 1000);
	std::tm tm{};
#if defined(_WIN32)
	gmtime_s(&tm, &seconds);
#else
	gmtime_r(&seconds, &tm);
#endif
	std::ostringstream out;
	out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
	return out.str();
}

constexpr char const* surface_text(araya::session_query::event_surface surface) {
	using araya::session_query::event_surface;
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

std::string availability_text(bool live, bool persisted) {
	if (live && persisted)
		return "live, persisted";
	if (live)
		return "live";
	if (persisted)
		return "persisted";
	return "unavailable";
}

// -- caller authority -------------------------------------------------------

struct caller_info {
	session_id id;
	std::optional<std::string> cwd;
};

caller_info require_caller(session_store& store, tool_context const& ctx) {
	if (ctx.session.empty())
		throw std::runtime_error("session query tools require an owning agent session");
	auto session = store.get(session_id{ctx.session});
	if (!session)
		throw std::runtime_error("session query: the calling session is not entered");
	return caller_info{session->id(), session->header().cwd};
}

bool authorized(caller_info const& caller, session_header const& header) {
	if (header.id == caller.id)
		return true;
	return caller.cwd && header.cwd && *caller.cwd == *header.cwd;
}

session_id target_id(boost::json::object const& object, caller_info const& caller) {
	if (auto explicit_id = opt_string(object, "session_id"); explicit_id && !explicit_id->empty())
		return session_id{*explicit_id};
	return caller.id;
}

std::string safe_error(std::exception const& error) {
	if (auto const* query = dynamic_cast<query_error const*>(&error)) {
		switch (query->code()) {
		case araya::session_query::query_error_code::stale_cursor:
			return "session history changed while paging; retry the complete search call";
		case araya::session_query::query_error_code::session_not_found:
			return "session not found";
		default:
			break;
		}
	}
	return error.what();
}

// -- presentation -----------------------------------------------------------

std::string title_of(session_query_service& query, session_id const& id) {
	auto title = query.read_title(id);
	return title ? *title : std::string("untitled");
}

std::string present_session_search(
	session_query_service& query,
	std::vector<araya::session_query::session_search_hit> const& hits,
	std::size_t cap) {
	if (hits.empty())
		return "No prior session matches found.";
	std::string out = "Session search results (" + std::to_string(hits.size()) + "):\n";
	for (std::size_t i = 0; i < hits.size(); ++i) {
		auto const& hit = hits[i];
		out += std::to_string(i + 1) + ". Session " + hit.session.header.id.value + " — " +
			   title_of(query, hit.session.header.id) + "\n";
		out += "   Created: " + format_time(hit.session.header.created_at) + "\n";
		out += "   Availability: " + availability_text(hit.session.live, hit.session.persisted) + "\n";
		out += "   Best match: seq " + std::to_string(hit.best_match.seq) + " | " + hit.best_match.type + " | " +
			   surface_text(hit.best_match.surface) + "\n";
		out += "   Snippet: " + hit.best_match.snippet + "\n";
	}
	if (hits.size() >= cap)
		out += "Result cap reached. Narrow the query or add filters to find additional matches.\n";
	return out;
}

std::string present_event_search(
	session_query_service& query,
	session_header const& header,
	std::vector<event_search_hit> const& hits,
	std::size_t cap) {
	std::string out = "Session " + header.id.value + " — " + title_of(query, header.id) + "\n";
	if (hits.empty()) {
		out += "No prior event matches found.";
		return out;
	}
	out += "Event search results (" + std::to_string(hits.size()) + "):\n";
	for (std::size_t i = 0; i < hits.size(); ++i) {
		auto const& hit = hits[i];
		out += std::to_string(i + 1) + ". seq " + std::to_string(hit.seq) + " | " + hit.type + " | " +
			   surface_text(hit.surface) + " | " + format_time(hit.time) + "\n";
		out += "   Snippet: " + hit.snippet + "\n";
	}
	if (hits.size() >= cap)
		out += "Result cap reached. Narrow the query or add filters to find additional matches.\n";
	return out;
}

std::string present_session_trace(session_query_service& query, araya::session_query::session_trace const& trace) {
	std::string out = "Session " + trace.target.header.id.value + " — " + title_of(query, trace.target.header.id) +
					  "\nCreated: " + format_time(trace.target.header.created_at) +
					  "\nAvailability: " + availability_text(trace.target.live, trace.target.persisted) + "\n";
	out += "Ancestors (nearest first):\n";
	if (trace.ancestors.empty())
		out += "- (none)\n";
	for (auto const& node : trace.ancestors)
		out += "- " + node.header.id.value + " — " + title_of(query, node.header.id) + "\n";
	out += "Descendants:\n";
	if (trace.descendants.empty())
		out += "- (none)\n";
	for (auto const& node : trace.descendants) {
		out += std::string(static_cast<std::size_t>(node.depth - 1), ' ');
		out += "- " + node.node.header.id.value + " — " + title_of(query, node.node.header.id) + "\n";
	}
	return out;
}

std::string present_event_trace(araya::session_query::event_trace const& trace) {
	std::string out = "Target: seq " + std::to_string(trace.target.seq) + " | " + trace.target.type + " | " +
					  surface_text(trace.target.surface) + "\n";
	if (trace.replaced_by)
		out += "Replaced by: seq " + std::to_string(trace.replaced_by->seq) + "\n";
	if (!trace.replacement_chain.empty()) {
		out += "Replacement chain:";
		for (auto const& event : trace.replacement_chain)
			out += " seq " + std::to_string(event.seq);
		out += "\n";
	}
	if (!trace.replaced_events.empty()) {
		out += "Events replaced by target:";
		for (auto const& event : trace.replaced_events)
			out += " seq " + std::to_string(event.seq);
		out += "\n";
	}
	return out;
}

std::string present_event_read(araya::session_query::event_read const& read) {
	std::string out = "Target event seq " + std::to_string(read.target.seq) + ":\n```json\n" +
					  boost::json::serialize(read.target.data) + "\n```\n";
	if (!read.before.empty()) {
		out += "Before:\n";
		for (auto const& event : read.before)
			out += "- seq " + std::to_string(event.seq) + " | " + event.type + "\n";
	}
	if (!read.after.empty()) {
		out += "After:\n";
		for (auto const& event : read.after)
			out += "- seq " + std::to_string(event.seq) + " | " + event.type + "\n";
	}
	return out;
}

// -- schemas ----------------------------------------------------------------

boost::json::value target_session_parameter() {
	return boost::json::object{
		{"session_id",
		 boost::json::object{
			 {"type", "string"}, {"description", "Target session id. Defaults to the current session."}}}};
}

boost::json::value session_search_schema() {
	boost::json::object properties{
		{"query",
		 boost::json::object{
			 {"type", "string"},
			 {"required", true},
			 {"description", "Literal full-text query over prior session history."}}},
		{"session_ids", boost::json::object{{"type", "array"}, {"description", "Optional session ids to include."}}},
		{"created_at_from",
		 boost::json::object{{"type", "string"}, {"description", "Inclusive ISO 8601 creation-time lower bound."}}},
		{"created_at_to",
		 boost::json::object{{"type", "string"}, {"description", "Inclusive ISO 8601 creation-time upper bound."}}},
		{"parent_session_ids",
		 boost::json::object{{"type", "array"}, {"description", "Optional direct parent session ids."}}},
		{"include_root_sessions",
		 boost::json::object{
			 {"type", "boolean"}, {"description", "Include sessions with no parent in the parent filter."}}},
		{"availability",
		 boost::json::object{
			 {"type", "array"},
			 {"description", "Require at least one selected source availability."},
			 {"items", boost::json::object{{"type", "string"}, {"enum", boost::json::array{"live", "persisted"}}}}}},
		{"event_seq_from",
		 boost::json::object{{"type", "number"}, {"description", "Inclusive event seq lower bound."}}},
		{"event_seq_to", boost::json::object{{"type", "number"}, {"description", "Inclusive event seq upper bound."}}},
		{"event_time_from",
		 boost::json::object{{"type", "string"}, {"description", "Inclusive ISO 8601 event-time lower bound."}}},
		{"event_time_to",
		 boost::json::object{{"type", "string"}, {"description", "Inclusive ISO 8601 event-time upper bound."}}},
		{"event_types", boost::json::object{{"type", "array"}, {"description", "Event types to include."}}},
		{"event_surfaces",
		 boost::json::object{
			 {"type", "array"},
			 {"description", "Event surfaces to include."},
			 {"items",
			  boost::json::object{
				  {"type", "string"}, {"enum", boost::json::array{"current", "shadowed", "log-only"}}}}}},
	};
	return boost::json::value{
		{"type", "object"}, {"properties", std::move(properties)}, {"required", boost::json::array{"query"}}};
}

boost::json::value event_search_schema() {
	boost::json::object properties{
		{"session_id", boost::json::object{{"type", "string"}, {"description", "Omit for the current session."}}},
		{"query",
		 boost::json::object{{"type", "string"}, {"required", true}, {"description", "Literal full-text query."}}},
		{"seq_from", boost::json::object{{"type", "number"}}},
		{"seq_to", boost::json::object{{"type", "number"}}},
		{"time_from", boost::json::object{{"type", "string"}}},
		{"time_to", boost::json::object{{"type", "string"}}},
		{"event_types", boost::json::object{{"type", "array"}}},
		{"surfaces",
		 boost::json::object{
			 {"type", "array"},
			 {"items",
			  boost::json::object{
				  {"type", "string"}, {"enum", boost::json::array{"current", "shadowed", "log-only"}}}}}},
	};
	return boost::json::value{
		{"type", "object"}, {"properties", std::move(properties)}, {"required", boost::json::array{"query"}}};
}

boost::json::value event_trace_schema() {
	boost::json::object properties = target_session_parameter().as_object();
	properties["seq"] = boost::json::object{{"type", "number"}, {"required", true}, {"description", "Event seq."}};
	return boost::json::value{
		{"type", "object"}, {"properties", std::move(properties)}, {"required", boost::json::array{"seq"}}};
}

boost::json::value event_read_schema() {
	boost::json::object properties = target_session_parameter().as_object();
	properties["seq"] = boost::json::object{{"type", "number"}, {"required", true}};
	properties["before"] = boost::json::object{{"type", "number"}, {"description", "Neighbour events before."}};
	properties["after"] = boost::json::object{{"type", "number"}, {"description", "Neighbour events after."}};
	return boost::json::value{
		{"type", "object"}, {"properties", std::move(properties)}, {"required", boost::json::array{"seq"}}};
}

// -- filter construction ----------------------------------------------------

std::vector<session_filter> build_session_filters(boost::json::object const& object) {
	std::vector<session_filter> filters;
	if (auto ids = string_array(object, "session_ids"); !ids.empty()) {
		session_filter filter;
		filter.clause = session_filter::kind::id;
		filter.values = std::move(ids);
		filters.push_back(std::move(filter));
	}
	if (auto from = opt_string(object, "created_at_from")) {
		session_filter filter;
		filter.clause = session_filter::kind::created_at;
		filter.from = parse_timestamp(*from);
		filters.push_back(std::move(filter));
	}
	if (auto to = opt_string(object, "created_at_to")) {
		session_filter filter;
		filter.clause = session_filter::kind::created_at;
		filter.to = parse_timestamp(*to);
		filters.push_back(std::move(filter));
	}
	auto parents = string_array(object, "parent_session_ids");
	bool root = bool_field(object, "include_root_sessions");
	if (!parents.empty() || root) {
		session_filter filter;
		filter.clause = session_filter::kind::parent;
		filter.values = std::move(parents);
		filter.allow_root = root;
		filters.push_back(std::move(filter));
	}
	if (auto values = string_array(object, "availability"); !values.empty()) {
		session_filter filter;
		filter.clause = session_filter::kind::availability;
		filter.values = std::move(values);
		filters.push_back(std::move(filter));
	}
	return filters;
}

std::vector<event_filter> build_event_filters(
	boost::json::object const& object,
	std::string_view seq_from_key,
	std::string_view seq_to_key,
	std::string_view time_from_key,
	std::string_view time_to_key,
	std::string_view types_key,
	std::string_view surfaces_key) {
	std::vector<event_filter> filters;
	if (auto from = opt_uint(object, seq_from_key)) {
		event_filter filter;
		filter.clause = event_filter::kind::seq;
		filter.from = static_cast<std::int64_t>(*from);
		filters.push_back(std::move(filter));
	}
	if (auto to = opt_uint(object, seq_to_key)) {
		event_filter filter;
		filter.clause = event_filter::kind::seq;
		filter.to = static_cast<std::int64_t>(*to);
		filters.push_back(std::move(filter));
	}
	if (auto from = opt_string(object, time_from_key)) {
		event_filter filter;
		filter.clause = event_filter::kind::time;
		filter.from = parse_timestamp(*from);
		filters.push_back(std::move(filter));
	}
	if (auto to = opt_string(object, time_to_key)) {
		event_filter filter;
		filter.clause = event_filter::kind::time;
		filter.to = parse_timestamp(*to);
		filters.push_back(std::move(filter));
	}
	if (auto types = string_array(object, types_key); !types.empty()) {
		event_filter filter;
		filter.clause = event_filter::kind::type;
		filter.values = std::move(types);
		filters.push_back(std::move(filter));
	}
	if (auto surfaces = string_array(object, surfaces_key); !surfaces.empty()) {
		event_filter filter;
		filter.clause = event_filter::kind::surface;
		filter.values = std::move(surfaces);
		filters.push_back(std::move(filter));
	}
	return filters;
}

// -- the plugin -------------------------------------------------------------

struct tool_session_query_plugin : araya::plugin {
	explicit tool_session_query_plugin(araya::plugin_config const& config) {
		araya::plugin_config_view const view(config);
		if (auto value = view.try_get(max_results_key))
			max_results_ = *value;
		if (max_results_ == 0)
			throw std::invalid_argument("tool-session-query: maxSearchResults must be at least 1");
	}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto tools = ctx.require<tools_service>(tools_key).shared();
		auto store = ctx.require<session_store>(sessions_key).shared();
		auto query = ctx.require<session_query_service>(session_query_key).shared();
		auto agent = ctx.require<araya::agent::agent_service>(araya::agent::agent_key).shared();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();

		araya::system_prompt::prompt_section section;
		section.name = "tool:session-query";
		section.order = araya::system_prompt::section_order("TOOL_SESSION_QUERY");
		section.text = std::string(k_prompt);
		prompts->section(ctx, std::move(section));

		auto const max_results = max_results_;

		auto session_search = [store, query, max_results](tool_context const& call) -> araya::task<tool_result> {
			try {
				auto const* object = call.arguments.if_object();
				if (!object)
					co_return error_result("Error: session_search requires an arguments object");
				auto const caller = require_caller(*store, call);
				auto const query_text = araya::util::json::get_string(*object, "query");
				if (araya::util::string::trim(query_text).empty())
					co_return error_result("Error: query must not be empty");
				// The caller's own session is never a prior-session hit; a
				// caller with no workspace can only see itself, so nothing.
				if (!caller.cwd)
					co_return text_result("No prior session matches found.");

				auto session_filters = build_session_filters(*object);
				auto event_filters = build_event_filters(
					*object,
					"event_seq_from",
					"event_seq_to",
					"event_time_from",
					"event_time_to",
					"event_types",
					"event_surfaces");
				session_filters.push_back(session_filter{
					.clause = session_filter::kind::cwd,
					.values = {*caller.cwd},
					.allow_root = false,
					.from = std::nullopt,
					.to = std::nullopt});

				session_search_request request;
				request.query = query_text;
				request.session_filters = std::move(session_filters);
				request.event_filters = std::move(event_filters);
				request.limit = max_results;
				auto page = query->search_sessions(request);
				std::erase_if(page.items, [&](auto const& hit) { return hit.session.header.id == caller.id; });
				co_return text_result(present_session_search(*query, page.items, max_results));
			} catch (std::exception const& error) {
				co_return error_result("Error: " + safe_error(error));
			}
		};

		auto event_search = [store, query, agent, max_results](tool_context const& call) -> araya::task<tool_result> {
			try {
				auto const* object = call.arguments.if_object();
				if (!object)
					co_return error_result("Error: session_event_search requires an arguments object");
				auto const caller = require_caller(*store, call);
				auto const query_text = araya::util::json::get_string(*object, "query");
				if (araya::util::string::trim(query_text).empty())
					co_return error_result("Error: query must not be empty");

				auto const target = target_id(*object, caller);
				auto source = query->read_session(target);
				if (!source)
					co_return error_result("Error: session not found");
				if (!authorized(caller, source->header))
					co_return error_result("Error: session target is outside the caller workspace");

				auto filters = build_event_filters(
					*object, "seq_from", "seq_to", "time_from", "time_to", "event_types", "surfaces");
				// Earlier events of the live session only: clamp to before
				// the current step so the model cannot search its own
				// in-flight turn.
				if (target == caller.id) {
					auto const* boundary = agent->turn_boundary(target);
					if (!boundary || !boundary->last_step_start_seq)
						co_return error_result("Error: session query requires an open model step");
					std::int64_t const upper = static_cast<std::int64_t>(*boundary->last_step_start_seq) - 1;
					if (upper < 0)
						co_return text_result(present_event_search(*query, source->header, {}, max_results));
					auto found = std::find_if(filters.begin(), filters.end(), [](event_filter const& filter) {
						return filter.clause == event_filter::kind::seq && filter.to.has_value();
					});
					if (found != filters.end())
						found->to = std::min(*found->to, upper);
					else {
						event_filter clamp;
						clamp.clause = event_filter::kind::seq;
						clamp.to = upper;
						filters.push_back(std::move(clamp));
					}
				}

				event_search_request request;
				request.session = target;
				request.query = query_text;
				request.filters = std::move(filters);
				request.limit = max_results;
				auto page = query->search_events(request);
				co_return text_result(present_event_search(*query, page.session, page.items, max_results));
			} catch (std::exception const& error) {
				co_return error_result("Error: " + safe_error(error));
			}
		};

		auto session_trace = [store, query](tool_context const& call) -> araya::task<tool_result> {
			try {
				auto const* object = call.arguments.if_object();
				if (!object)
					co_return error_result("Error: session_trace requires an arguments object");
				auto const caller = require_caller(*store, call);
				auto const target = target_id(*object, caller);
				auto trace = query->trace_session(target);
				if (!trace)
					co_return error_result("Error: session not found");
				if (!authorized(caller, trace->target.header))
					co_return error_result("Error: session target is outside the caller workspace");
				std::erase_if(
					trace->ancestors, [&](lineage_node const& node) { return !authorized(caller, node.header); });
				std::erase_if(trace->descendants, [&](descendant_node const& node) {
					return !authorized(caller, node.node.header);
				});
				co_return text_result(present_session_trace(*query, *trace));
			} catch (std::exception const& error) {
				co_return error_result("Error: " + safe_error(error));
			}
		};

		auto event_trace = [store, query](tool_context const& call) -> araya::task<tool_result> {
			try {
				auto const* object = call.arguments.if_object();
				if (!object)
					co_return error_result("Error: session_event_trace requires an arguments object");
				auto const caller = require_caller(*store, call);
				auto const target = target_id(*object, caller);
				auto source = query->read_session(target);
				if (!source)
					co_return error_result("Error: session not found");
				if (!authorized(caller, source->header))
					co_return error_result("Error: session target is outside the caller workspace");
				auto const seq = opt_uint(*object, "seq");
				if (!seq)
					co_return error_result("Error: seq is required");
				auto trace = query->trace_event(target, *seq);
				if (!trace)
					co_return error_result("Error: event not found");
				co_return text_result(present_event_trace(*trace));
			} catch (std::exception const& error) {
				co_return error_result("Error: " + safe_error(error));
			}
		};

		auto event_read = [store, query](tool_context const& call) -> araya::task<tool_result> {
			try {
				auto const* object = call.arguments.if_object();
				if (!object)
					co_return error_result("Error: session_event_read requires an arguments object");
				auto const caller = require_caller(*store, call);
				auto const target = target_id(*object, caller);
				auto source = query->read_session(target);
				if (!source)
					co_return error_result("Error: session not found");
				if (!authorized(caller, source->header))
					co_return error_result("Error: session target is outside the caller workspace");
				auto const seq = opt_uint(*object, "seq");
				if (!seq)
					co_return error_result("Error: seq is required");
				auto const before = opt_uint(*object, "before").value_or(0);
				auto const after = opt_uint(*object, "after").value_or(0);
				auto read =
					query->read_event(target, *seq, static_cast<unsigned>(before), static_cast<unsigned>(after));
				if (!read)
					co_return error_result("Error: event not found");
				co_return text_result(present_event_read(*read));
			} catch (std::exception const& error) {
				co_return error_result("Error: " + safe_error(error));
			}
		};

		tools->register_tool(
			ctx,
			tool_definition{
				"session_search",
				"Search prior sessions in the current workspace for relevant work. Returns ranked session hits with a "
				"best-matching snippet.",
				session_search_schema()},
			std::move(session_search));
		tools->register_tool(
			ctx,
			tool_definition{
				"session_event_search",
				"Search the events of one session (the current session by default) for earlier relevant events. "
				"Live-session search excludes the in-flight turn.",
				event_search_schema()},
			std::move(event_search));
		tools->register_tool(
			ctx,
			tool_definition{
				"session_trace",
				"Show the lineage (ancestors and descendants) of a session.",
				target_session_parameter()},
			std::move(session_trace));
		tools->register_tool(
			ctx,
			tool_definition{
				"session_event_trace", "Show the replacement relationships of one event.", event_trace_schema()},
			std::move(event_trace));
		tools->register_tool(
			ctx,
			tool_definition{
				"session_event_read",
				"Read one event exactly, with optional neighbouring events.",
				event_read_schema()},
			std::move(event_read));
		co_return;
	}

private:
	std::uint64_t max_results_ = 100;
};

std::unique_ptr<araya::plugin> make_tool_session_query(araya::plugin_config const& config) {
	return std::make_unique<tool_session_query_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"session-query", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
	{araya::service_id{"agent", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor
	g_descriptor{"tool-session-query", g_deps, g_provs, &make_tool_session_query, g_config};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_session_query
