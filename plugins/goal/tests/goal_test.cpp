#include <catch2/catch_test_macros.hpp>

#include "araya/goal/goal.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "support/plugin_harness.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/json/object.hpp>

#include <memory>
#include <string>
#include <utility>

namespace {

using namespace araya::goal;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec goal_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::goal::plugin_descriptor(), std::move(cfg));
	}
};

struct rig {
	std::shared_ptr<goal_service> goals;
	std::shared_ptr<araya::session::session_store> store;

	araya::task<void> mount(araya::runtime& rt, harness& h) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.goal_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		goals = root.require<goal_service>(goals_key).shared();
		store->create(root, araya::session::session_id{"s1"}, {});
	}
};

araya::session::session_id const s1{"s1"};

} // namespace

TEST_CASE("create makes an active armed goal and get reads it back") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		CHECK_FALSE(r.goals->get(s1).has_value());
		auto goal = r.goals->create(s1, "  ship the feature  ");
		CHECK(goal.objective == "ship the feature");
		CHECK(goal.phase == goal_phase::active);
		CHECK(goal.revision == 1);
		CHECK(goal.max_goal_rounds == 256);
		CHECK(goal.activation == goal_activation::armed);
		CHECK(r.goals->activation(s1) == goal_activation::armed);

		auto read = r.goals->get(s1);
		REQUIRE(read.has_value());
		CHECK(read->id == goal.id);

		// A second create over a non-complete goal is rejected.
		CHECK_THROWS_AS(r.goals->create(s1, "another"), goal_error);
	});
}

TEST_CASE("mutations are compare-and-set and advance the revision") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto goal = r.goals->create(s1, "objective");

		CHECK_THROWS_AS(r.goals->edit(s1, goal_ref{goal.id, 99}, "x", std::nullopt), goal_error);
		CHECK_THROWS_AS(r.goals->edit(s1, goal_ref{"other", 1}, "x", std::nullopt), goal_error);

		auto edited = r.goals->edit(s1, goal_ref{goal.id, goal.revision}, "new objective", std::nullopt);
		CHECK(edited.revision == 2);
		CHECK(edited.objective == "new objective");
		// Editing alone keeps the goal active and armed.
		CHECK(edited.phase == goal_phase::active);
		CHECK(edited.activation == goal_activation::armed);
	});
}

TEST_CASE("pause, resume, complete, block, and clear transition phases") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto goal = r.goals->create(s1, "objective");

		auto paused = r.goals->pause(s1, goal_ref{goal.id, goal.revision});
		CHECK(paused.phase == goal_phase::paused);
		CHECK(paused.activation == goal_activation::disarmed);

		auto resumed = r.goals->resume(s1, goal_ref{goal.id, paused.revision});
		CHECK(resumed.phase == goal_phase::active);
		CHECK(resumed.activation == goal_activation::armed);

		auto blocked = r.goals->block(
			s1, goal_ref{goal.id, resumed.revision}, goal_block_reason{"needs-input", "waiting on a decision"});
		CHECK(blocked.phase == goal_phase::blocked);
		REQUIRE(blocked.blocked_reason.has_value());
		CHECK(blocked.blocked_reason->code == "needs-input");

		auto completed = r.goals->complete(s1, goal_ref{goal.id, blocked.revision});
		CHECK(completed.phase == goal_phase::complete);
		CHECK(completed.activation == goal_activation::disarmed);

		// A completed goal may be cleared.
		auto tombstone = r.goals->clear(s1, goal_ref{goal.id, completed.revision});
		CHECK(tombstone.revision == completed.revision + 1);
		CHECK_FALSE(r.goals->get(s1).has_value());

		// After a clear a fresh goal may be created.
		auto next = r.goals->create(s1, "second objective");
		CHECK(next.revision == 1);
		CHECK(next.id != goal.id);
	});
}

TEST_CASE("disarm removes continuation authority without touching phase") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto goal = r.goals->create(s1, "objective");
		CHECK(goal.activation == goal_activation::armed);

		auto disarmed = r.goals->disarm(s1);
		REQUIRE(disarmed.has_value());
		CHECK(disarmed->activation == goal_activation::disarmed);
		CHECK(disarmed->phase == goal_phase::active);

		// Re-arming an already active goal is allowed and bumps the revision.
		auto rearmed = r.goals->resume(s1, goal_ref{goal.id, goal.revision});
		CHECK(rearmed.activation == goal_activation::armed);
	});
}

TEST_CASE("goal rounds advance the projection counter and reject out-of-order rounds") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto goal = r.goals->create(s1, "objective");

		auto session = r.store->get(s1);
		REQUIRE(session);
		auto round_message = [&](std::uint64_t round) {
			return araya::llm_bridge::user_message_data(
				"g" + std::to_string(round),
				"round " + std::to_string(round),
				araya::llm_bridge::message_source(
					"goal", {{"goalId", goal.id}, {"revision", goal.revision}, {"round", round}}));
		};
		session->append("user/message", round_message(1));
		session->append("user/message", round_message(2));

		auto read = r.goals->get(s1);
		REQUIRE(read.has_value());
		CHECK(read->rounds_started == 2);

		// A skipped round poisons the strict replay and reads fail loudly.
		session->append("user/message", round_message(4));
		CHECK_THROWS_AS(r.goals->get(s1), goal_error);
	});
}

TEST_CASE("goal state restores from the replayed session log") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);
		auto goal = r.goals->create(s1, "durable objective");
		r.goals->pause(s1, goal_ref{goal.id, goal.revision});

		auto session = r.store->get(s1);
		REQUIRE(session);
		auto log = session->log();
		r.store->dispose(s1);
		CHECK_FALSE(r.goals->get(s1).has_value());

		auto restored = r.store->prepare(
			s1, araya::session::create_session_options{.seed = log, .inherited_event_count = log.size()});
		r.store->enter(restored);
		r.store->announce(*restored);

		auto read = r.goals->get(s1);
		REQUIRE(read.has_value());
		CHECK(read->id == goal.id);
		CHECK(read->phase == goal_phase::paused);
		CHECK(read->revision == 2);
	});
}
