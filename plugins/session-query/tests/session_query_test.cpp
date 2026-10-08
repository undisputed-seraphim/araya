#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session-query/query.hpp"
#include "araya/session/store.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace araya::session_query;
using araya::session::session_event;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec query_spec() { return spec(&araya::session_query::plugin_descriptor()); }
};

struct rig {
	std::shared_ptr<session_store> store;
	std::shared_ptr<session_query_service> query;
	std::optional<araya::plugin_context> owner;

	araya::task<void> mount(araya::runtime& rt, harness& h) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.query_spec());
		co_await rt.wait_idle();
		owner.emplace(rt.root_context());
		store = owner->require<session_store>(sessions_key).shared();
		query = owner->require<session_query_service>(session_query_key).shared();
	}

	araya::session::session& create(
		std::string const& id,
		std::optional<std::string> cwd = std::nullopt,
		std::optional<session_id> parent = std::nullopt) {
		araya::session::create_session_options options;
		options.cwd = std::move(cwd);
		options.parent_session = std::move(parent);
		return *store->create(*owner, session_id{id}, std::move(options));
	}
};

session_event user_event(araya::session::session& s, std::string id, std::string text) {
	s.append(
		"user/message",
		boost::json::value{
			{"id", std::move(id)},
			{"role", "user"},
			{"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", std::move(text)}}}}});
	return s.log().back();
}

session_event assistant_event(araya::session::session& s, std::string id, std::string text) {
	s.append(
		"assistant/message",
		boost::json::value{
			{"message",
			 boost::json::object{
				 {"id", std::move(id)},
				 {"role", "assistant"},
				 {"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", std::move(text)}}}}}}});
	return s.log().back();
}

TEST_CASE("extract_event_text reads message text and skips reasoning") {
	session_event user;
	user.type = "user/message";
	user.data = boost::json::value{
		{"id", "u1"}, {"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", "hello world"}}}}};
	CHECK(extract_event_text(user) == "hello world");

	session_event assistant;
	assistant.type = "assistant/message";
	assistant.data = boost::json::value{
		{"message",
		 boost::json::object{
			 {"id", "a1"},
			 {"content",
			  boost::json::array{
				  boost::json::object{{"type", "reasoning"}, {"text", "hidden"}},
				  boost::json::object{{"type", "text"}, {"text", "visible"}},
				  boost::json::object{{"type", "tool_call"}, {"name", "ls"}, {"arguments", R"({"path":"."})"}}}}}}};
	auto const text = extract_event_text(assistant);
	CHECK(text.find("hidden") == std::string::npos);
	CHECK(text.find("visible") != std::string::npos);
	CHECK(text.find("ls") != std::string::npos);
}

TEST_CASE("classify_surfaces marks current, shadowed, and log-only") {
	std::vector<session_event> events(3);
	events[0].seq = 0;
	events[0].type = "user/message";
	events[0].data = boost::json::value{
		{"id", "m0"}, {"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", "a"}}}}};
	events[1].seq = 1;
	events[1].type = "turn/start";
	events[1].data = boost::json::value{{"turn", 1}};
	events[2].seq = 2;
	events[2].type = "surface/replace";
	events[2].data = boost::json::value{
		{"start_seq", 0},
		{"end_seq", 1},
		{"message",
		 boost::json::object{
			 {"id", "r"},
			 {"role", "user"},
			 {"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", "b"}}}}}}};

	auto const surfaces = classify_surfaces(events);
	REQUIRE(surfaces.size() == 3);
	CHECK(surfaces[0] == event_surface::shadowed);
	CHECK(surfaces[1] == event_surface::log_only);
	CHECK(surfaces[2] == event_surface::current);
}

TEST_CASE("engine lists, searches, reads, and titles live sessions") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto& s = r.create("ses_a", std::string("/work"));
		user_event(s, "u1", "please fix the parser bug in foo");
		assistant_event(s, "a1", "I will inspect foo now");

		auto const sessions = r.query->list_sessions();
		REQUIRE(sessions.size() == 1);
		CHECK(sessions.front().header.id.value == "ses_a");
		CHECK(sessions.front().live);

		session_search_request request;
		request.query = "parser bug";
		auto const page = r.query->search_sessions(request);
		REQUIRE(page.items.size() == 1);
		CHECK(page.items.front().session.header.id.value == "ses_a");
		CHECK(page.items.front().best_match.snippet.find("parser") != std::string::npos);

		event_search_request event_request;
		event_request.session = session_id{"ses_a"};
		event_request.query = "inspect foo";
		auto const events = r.query->search_events(event_request);
		REQUIRE(events.items.size() == 1);
		CHECK(events.items.front().type == "assistant/message");

		auto const title = r.query->read_title(session_id{"ses_a"});
		REQUIRE(title.has_value());
		CHECK(title->find("parser bug") != std::string::npos);

		auto const read = r.query->read_event(session_id{"ses_a"}, 0, 1, 1);
		REQUIRE(read.has_value());
		CHECK(read->target.seq == 0);
		CHECK(read->before.empty());
		REQUIRE(read->after.size() == 1);
		CHECK(read->after.front().seq == 1);
	});
}

TEST_CASE("filters select by cwd and availability") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		r.create("ses_a", std::string("/work"));
		r.create("ses_b", std::string("/elsewhere"));

		std::vector<session_filter> filters{{.clause = session_filter::kind::cwd, .values = {"/work"}}};
		auto const work = r.query->filter_sessions(filters);
		REQUIRE(work.size() == 1);
		CHECK(work.front().header.id.value == "ses_a");

		std::vector<session_filter> persisted{{.clause = session_filter::kind::availability, .values = {"persisted"}}};
		CHECK(r.query->filter_sessions(persisted).empty());
	});
}

TEST_CASE("pagination returns a cursor") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		user_event(r.create("ses_a"), "u", "shared keyword");
		user_event(r.create("ses_b"), "u", "shared keyword");

		session_search_request request;
		request.query = "shared";
		request.limit = 1;
		auto first = r.query->search_sessions(request);
		REQUIRE(first.items.size() == 1);
		CHECK_FALSE(first.next_cursor.empty());
		request.cursor = first.next_cursor;
		auto second = r.query->search_sessions(request);
		REQUIRE(second.items.size() == 1);
		CHECK(second.next_cursor.empty());
		CHECK(first.items.front().session.header.id.value != second.items.front().session.header.id.value);
	});
}

TEST_CASE("lineage trace walks ancestors and descendants") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		r.create("root");
		r.create("child", std::nullopt, session_id{"root"});
		r.create("grandchild", std::nullopt, session_id{"child"});

		auto const trace = r.query->trace_session(session_id{"root"});
		REQUIRE(trace.has_value());
		CHECK(trace->ancestors.empty());
		REQUIRE(trace->descendants.size() == 2);
	});
}

TEST_CASE("event trace finds the replacing event") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto& s = r.create("ses");
		user_event(s, "u", "original");
		assistant_event(s, "a", "reply");
		s.append(
			"surface/replace",
			boost::json::value{
				{"start_seq", 0},
				{"end_seq", 2},
				{"message",
				 boost::json::object{
					 {"id", "r"},
					 {"role", "assistant"},
					 {"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", "summary"}}}}}}});

		auto const shadowed = r.query->trace_event(session_id{"ses"}, 0);
		REQUIRE(shadowed.has_value());
		REQUIRE(shadowed->replaced_by.has_value());
		CHECK(shadowed->replaced_by->seq == 2);

		auto const replacer = r.query->trace_event(session_id{"ses"}, 2);
		REQUIRE(replacer.has_value());
		CHECK(replacer->replaced_events.size() == 2);
	});
}

} // namespace
