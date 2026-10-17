#include <catch2/catch_test_macros.hpp>

#include "araya/persistence/persistence.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session-query-sqlite/sqlite.hpp"
#include "araya/session-query/query.hpp"
#include "araya/session/store.hpp"

#include "support/plugin_harness.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

using namespace araya::session_query;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;

struct harness : araya_test::plugin_harness {
	std::filesystem::path root;
	std::filesystem::path db;
	int counter = 0;

	explicit harness() {
		root = std::filesystem::temp_directory_path() /
			   ("araya-sqlite-query-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
		db = root / "index.sqlite3";
		std::filesystem::remove_all(root);
	}

	~harness() { std::filesystem::remove_all(root); }

	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec persistence_spec() {
		return spec(&araya::persistence::plugin_descriptor(), {{"root", root.string()}});
	}
	araya::component_spec query_spec() {
		return spec(&araya::session_query_sqlite::plugin_descriptor(), {{"path", db.string()}});
	}
};

struct rig {
	std::shared_ptr<session_store> store;
	std::shared_ptr<session_query_service> query;
	std::optional<araya::plugin_context> owner;

	araya::task<void> mount(araya::runtime& rt, harness& h) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.persistence_spec());
		co_await rt.mount(h.query_spec());
		co_await rt.wait_idle();
		owner.emplace(rt.root_context());
		store = owner->require<session_store>(sessions_key).shared();
		query = owner->require<session_query_service>(session_query_key).shared();
	}

	araya::session::session& create(std::string const& id, std::optional<std::string> cwd = std::nullopt) {
		araya::session::create_session_options options;
		options.cwd = std::move(cwd);
		return *store->create(*owner, session_id{id}, std::move(options));
	}
};

void append_user(araya::session::session& session, std::string id, std::string text) {
	session.append(
		"user/message",
		boost::json::value{
			{"id", std::move(id)},
			{"role", "user"},
			{"content", boost::json::array{boost::json::object{{"type", "text"}, {"text", std::move(text)}}}}});
}

TEST_CASE("sqlite search indexes live sessions with FTS ranking") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		append_user(r.create("s1", std::string("/work")), "u", "hello world alpha");
		append_user(r.create("s2", std::string("/work")), "u", "hello world beta");
		append_user(r.create("s3", std::string("/elsewhere")), "u", "hello world gamma");

		session_search_request request;
		request.query = "hello world";
		session_filter cwd_filter;
		cwd_filter.clause = session_filter::kind::cwd;
		cwd_filter.values = {"/work"};
		request.session_filters.push_back(cwd_filter);
		auto const page = r.query->search_sessions(request);
		REQUIRE(page.items.size() == 2);
		std::set<std::string> ids;
		for (auto const& hit : page.items)
			ids.insert(hit.session.header.id.value);
		CHECK(ids == std::set<std::string>{"s1", "s2"});
		CHECK(page.items.front().best_match.type == "user/message");

		auto const filtered = r.query->filter_sessions({cwd_filter});
		REQUIRE(filtered.size() == 2);

		event_search_request events;
		events.session = session_id{"s1"};
		events.query = "alpha";
		auto const hits = r.query->search_events(events);
		REQUIRE(hits.items.size() == 1);
		CHECK(hits.items.front().seq == 0);
		CHECK(hits.items.front().snippet.find("alpha") != std::string::npos);
	});
}

TEST_CASE("sqlite search paginates with a stale-aware cursor") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		append_user(r.create("a", std::string("/work")), "u", "shared keyword");
		append_user(r.create("b", std::string("/work")), "u", "shared keyword");

		session_search_request request;
		request.query = "shared";
		request.limit = 1;
		auto first = r.query->search_sessions(request);
		REQUIRE(first.items.size() == 1);
		REQUIRE_FALSE(first.next_cursor.empty());
		request.cursor = first.next_cursor;
		auto second = r.query->search_sessions(request);
		REQUIRE(second.items.size() == 1);
		CHECK(second.next_cursor.empty());
		CHECK(first.items.front().session.header.id.value != second.items.front().session.header.id.value);

		// An append changes the generation, so the old cursor is stale.
		append_user(*r.store->get(session_id{"a"}), "u2", "shared keyword again");
		request.cursor = first.next_cursor;
		CHECK_THROWS_AS(r.query->search_sessions(request), query_error);
	});
}

TEST_CASE("sqlite search falls back to persisted sessions") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto& s = r.create("p1", std::string("/work"));
		append_user(s, "u", "persisted needle");
		co_await r.store->flush(session_id{"p1"});
		r.store->dispose(session_id{"p1"});

		session_search_request request;
		request.query = "needle";
		auto const page = r.query->search_sessions(request);
		REQUIRE(page.items.size() == 1);
		CHECK(page.items.front().session.header.id.value == "p1");
		CHECK(page.items.front().session.persisted);
		CHECK_FALSE(page.items.front().session.live);

		event_search_request events;
		events.session = session_id{"p1"};
		events.query = "needle";
		CHECK(r.query->search_events(events).items.size() == 1);
	});
}

TEST_CASE("sqlite search rejects a junk cursor and an unknown session") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		append_user(r.create("s1", std::string("/work")), "u", "text");

		session_search_request request;
		request.query = "text";
		request.cursor = "garbage";
		CHECK_THROWS_AS(r.query->search_sessions(request), query_error);

		event_search_request events;
		events.session = session_id{"missing"};
		events.query = "text";
		CHECK_THROWS_AS(r.query->search_events(events), query_error);
	});
}

} // namespace
