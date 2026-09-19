#include <catch2/catch_test_macros.hpp>

#include "araya/persistence/persistence.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/json/value.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace {

using araya::persistence::make_jsonl_backend;
using araya::persistence::persistence_key;
using araya::persistence::session_persistence;
using araya::persistence::stored_session;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;

struct temp_dir {
	temp_dir() {
		path = std::filesystem::temp_directory_path() / ("araya-persistence-" + std::to_string(++counter));
		std::filesystem::create_directories(path);
	}
	~temp_dir() {
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}

	std::filesystem::path path;
	static inline int counter = 0;
};

boost::json::value user_message(std::string_view id, std::string_view text) {
	return {
		{"id", std::string(id)},
		{"role", "user"},
		{"content", boost::json::array{{{"type", "text"}, {"text", std::string(text)}}}},
	};
}

araya::session::session_header test_header(std::string id) {
	araya::session::session_header h;
	h.id = session_id{std::move(id)};
	h.created_at = 123456;
	h.cwd = "/somewhere";
	h.parent_session = session_id{"parent"};
	h.agent_preset = "p1";
	return h;
}

TEST_CASE("jsonl backend round-trips a header and events") {
	temp_dir dir;
	auto backend = make_jsonl_backend(dir.path);

	auto header = test_header("s1");
	backend->attach(header);
	backend->append(header.id, araya::session::session_event{0, 100, "user/message", user_message("m1", "hi"), false});
	backend->append(
		header.id, araya::session::session_event{1, 200, "custom/thing", boost::json::value{{"a", 1}}, true});
	backend->flush(header.id);

	auto stored = backend->read(header.id);
	REQUIRE(stored.has_value());
	CHECK(stored->header.id.value == "s1");
	CHECK(stored->header.created_at == 123456);
	CHECK(stored->header.cwd == "/somewhere");
	CHECK(stored->header.parent_session == session_id{"parent"});
	CHECK(stored->header.agent_preset == "p1");
	REQUIRE(stored->events.size() == 2);
	CHECK(stored->events[0].seq == 0);
	CHECK(stored->events[0].type == "user/message");
	CHECK(stored->events[0].data == user_message("m1", "hi"));
	CHECK(stored->events[1].seq == 1);
	CHECK(stored->events[1].ignorable);
	CHECK(stored->events[1].data.at("a") == 1);

	backend->detach(header.id);
}

TEST_CASE("attach truncates a torn trailing record") {
	temp_dir dir;
	auto backend = make_jsonl_backend(dir.path);

	auto header = test_header("s1");
	backend->attach(header);
	backend->append(header.id, araya::session::session_event{0, 100, "user/message", user_message("m1", "hi"), false});
	backend->flush(header.id);
	backend->detach(header.id);

	// A crashed write: a half-written record with no trailing newline.
	auto torn_path = dir.path / "7331" / "session.v1.jsonl";
	{
		std::ofstream out(torn_path, std::ios::app);
		out << "{\"seq\":1,\"time\":200,\"type\":\"user/mess";
	}

	// Reattaching truncates the torn tail; the next append and flush
	// land on a clean boundary and only the complete record survives.
	backend->attach(header);
	backend->append(
		header.id, araya::session::session_event{1, 300, "custom/thing", boost::json::value{{"b", 2}}, false});
	backend->flush(header.id);
	backend->detach(header.id);

	auto stored = backend->read(header.id);
	REQUIRE(stored.has_value());
	REQUIRE(stored->events.size() == 2);
	CHECK(stored->events[0].type == "user/message");
	CHECK(stored->events[1].seq == 1);
	CHECK(stored->events[1].type == "custom/thing");
}

TEST_CASE("read is nullopt for unknown sessions and list enumerates stored ids") {
	temp_dir dir;
	auto backend = make_jsonl_backend(dir.path);

	CHECK(!backend->read(session_id{"nope"}).has_value());

	auto a = test_header("alpha");
	auto b = test_header("beta");
	backend->attach(a);
	backend->attach(b);
	backend->flush(a.id);
	backend->flush(b.id);
	backend->detach(a.id);
	backend->detach(b.id);

	// A stray non-hex directory is not ours to report.
	std::filesystem::create_directories(dir.path / "not-hex!");

	auto ids = backend->list();
	CHECK(std::find(ids.begin(), ids.end(), session_id{"alpha"}) != ids.end());
	CHECK(std::find(ids.begin(), ids.end(), session_id{"beta"}) != ids.end());
	CHECK(std::find(ids.begin(), ids.end(), session_id{"not-hex!"}) == ids.end());
}

TEST_CASE("an unsupported format version throws on read") {
	temp_dir dir;
	auto backend = make_jsonl_backend(dir.path);

	auto header = test_header("s1");
	backend->attach(header);
	backend->flush(header.id);
	backend->detach(header.id);

	// Rewrite the header record with a future version.
	auto file = dir.path / "7331" / "session.v1.jsonl";
	std::ofstream out(file, std::ios::trunc);
	out << "{\"type\":\"session\",\"version\":2,\"id\":\"s1\",\"created_at\":1,\"is_seeded\":false,"
		   "\"delegation_depth\":0,\"origin\":\"none\"}\n";

	CHECK_THROWS_AS(backend->read(header.id), std::runtime_error);
}

struct harness {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());
	temp_dir dir;

	template <typename Fn>
	void run(Fn&& fn) {
		struct driver {
			std::decay_t<Fn> fn;
			harness* self;
			araya::task<void> operator()() { co_await fn(*self->rt); }
		};
		boost::asio::co_spawn(io.get_executor(), driver{std::forward<Fn>(fn), this}, boost::asio::detached);
		io.run();
		io.restart();
	}

	araya::component_spec spec(araya::plugin_descriptor const* d, araya::plugin_config cfg = {}) {
		return araya::component_spec{
			std::shared_ptr<araya::plugin_descriptor>(const_cast<araya::plugin_descriptor*>(d), [](auto*) {}),
			std::move(cfg),
			nullptr,
			""};
	}
};

TEST_CASE("the plugin persists appends through the flush barrier and restores via prepare") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec(&araya::session::plugin_descriptor()));
		co_await rt.mount(h.spec(&araya::persistence::plugin_descriptor(), {{"root", h.dir.path.string()}}));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto store = root_ctx.require<session_store>(sessions_key);
		auto backend = root_ctx.require<session_persistence>(persistence_key);

		auto s = store->create(root_ctx, session_id{"s1"}, {});
		s->append("user/message", user_message("m1", "first"));
		s->append("custom/thing", boost::json::value{{"x", 1}});
		co_await s->flush();
		store->dispose(session_id{"s1"});
		co_await rt.wait_idle();

		// After the queued detach listener, the file is closed and
		// readable.
		std::optional<stored_session> stored;
		co_await rt.run_on_strand([&] { stored = backend->read(session_id{"s1"}); });
		REQUIRE(stored.has_value());
		REQUIRE(stored->events.size() == 2);

		// The restore path: prepare with the stored log, enter, announce.
		auto restored = store->prepare(
			session_id{"s1"},
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
		store->enter(restored);
		store->announce(*restored);

		// The surface re-folds from the seed; appends resume after it.
		CHECK(restored->surface().messages().size() == 1);
		auto seq = restored->append("user/message", user_message("m2", "second"));
		CHECK(seq == 2);
		co_await restored->flush();
		store->dispose(session_id{"s1"});
		co_await rt.wait_idle();

		co_await rt.run_on_strand([&] { stored = backend->read(session_id{"s1"}); });
		REQUIRE(stored.has_value());
		CHECK(stored->events.size() == 3);
	});
}

TEST_CASE("events for sessions created before the plugin mounts are not persisted") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec(&araya::session::plugin_descriptor()));
		co_await rt.wait_idle();

		auto root_ctx = rt.root_context();
		auto store = root_ctx.require<session_store>(sessions_key);

		// Created while persistence is absent: nothing is written.
		auto s = store->create(root_ctx, session_id{"early"}, {});
		s->append("user/message", user_message("m1", "unpersisted"));
		co_await s->flush();

		co_await rt.mount(h.spec(&araya::persistence::plugin_descriptor(), {{"root", h.dir.path.string()}}));
		co_await rt.wait_idle();

		auto backend = root_ctx.require<session_persistence>(persistence_key);
		std::optional<stored_session> stored;
		co_await rt.run_on_strand([&] { stored = backend->read(session_id{"early"}); });
		CHECK(!stored.has_value());
	});
}

} // namespace
