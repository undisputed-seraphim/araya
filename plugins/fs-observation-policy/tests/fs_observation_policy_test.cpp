#include <catch2/catch_test_macros.hpp>

#include "araya/fs-observation-policy/fs_observation_policy.hpp"
#include "araya/fs/events.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include "support/plugin_harness.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <string>
#include <utility>

namespace {

using araya::fs::fs_edit_decision;
using araya::fs::fs_edit_intent;
using araya::fs::fs_observation;
using araya::fs::fs_observed_msg;
using araya::fs::fs_target;
using araya::fs::fs_write_decision;
using araya::fs::fs_write_intent;

struct harness : araya_test::plugin_harness {
	araya::component_spec policy_spec() { return spec(&araya::fs_observation_policy::plugin_descriptor()); }
};

fs_target at(std::string owner = "s1") { return fs_target{std::move(owner), "/tmp/x", "x"}; }

// Dispatch a waterfall on the control strand and return the final message.
template <class Message>
araya::task<Message>
waterfall(araya::runtime& rt, araya::event_key<Message, araya::dispatch_mode::waterfall> key, Message msg) {
	co_return co_await boost::asio::co_spawn(
		rt.bus()->executor(),
		[&]() -> araya::task<Message> { co_return co_await rt.bus()->dispatch(key, msg); },
		boost::asio::use_awaitable);
}

// Emit one observation on the control strand.
araya::task<void> observe(araya::runtime& rt, fs_target target, bool present, std::string version) {
	co_await boost::asio::co_spawn(
		rt.bus()->executor(),
		[&]() -> araya::task<void> {
			rt.bus()->dispatch(
				araya::fs::observed_key,
				fs_observed_msg{std::move(target), fs_observation{present, std::move(version)}});
			co_return;
		},
		boost::asio::use_awaitable);
}

} // namespace

TEST_CASE("without a policy listener every intent is unconstrained") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto write = co_await waterfall(rt, araya::fs::write_intent_key, fs_write_intent{.target = at()});
		CHECK(write.decision == fs_write_decision::unconstrained);
		auto edit = co_await waterfall(rt, araya::fs::edit_intent_key, fs_edit_intent{.target = at()});
		CHECK(edit.error.empty());
		CHECK(edit.decision == fs_edit_decision::unconstrained);
		co_return;
	});
}

TEST_CASE("write intent is create-only until a target is observed present") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.policy_spec());
		co_await rt.wait_idle();

		auto unseen = co_await waterfall(rt, araya::fs::write_intent_key, fs_write_intent{.target = at()});
		CHECK(unseen.decision == fs_write_decision::create_if_absent);

		co_await observe(rt, at(), false, "");
		auto absent = co_await waterfall(rt, araya::fs::write_intent_key, fs_write_intent{.target = at()});
		CHECK(absent.decision == fs_write_decision::create_if_absent);

		co_await observe(rt, at(), true, "v1");
		auto present = co_await waterfall(rt, araya::fs::write_intent_key, fs_write_intent{.target = at()});
		CHECK(present.decision == fs_write_decision::replace_if_version);
		CHECK(present.version == "v1");
		co_return;
	});
}

TEST_CASE("edit intent denies unseen and absent, guards present") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.policy_spec());
		co_await rt.wait_idle();

		auto unseen = co_await waterfall(rt, araya::fs::edit_intent_key, fs_edit_intent{.target = at()});
		CHECK(unseen.error.find("has not been read") != std::string::npos);

		co_await observe(rt, at(), false, "");
		auto absent = co_await waterfall(rt, araya::fs::edit_intent_key, fs_edit_intent{.target = at()});
		CHECK(absent.error.find("not found") != std::string::npos);

		co_await observe(rt, at(), true, "v2");
		auto present = co_await waterfall(rt, araya::fs::edit_intent_key, fs_edit_intent{.target = at()});
		CHECK(present.error.empty());
		CHECK(present.decision == fs_edit_decision::replace_if_version);
		CHECK(present.version == "v2");
		co_return;
	});
}

TEST_CASE("observed state is keyed per owner and dropped without an owner") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.policy_spec());
		co_await rt.wait_idle();

		co_await observe(rt, at("s1"), true, "v1");
		// Another session sees the target as unseen.
		auto other = co_await waterfall(rt, araya::fs::write_intent_key, fs_write_intent{.target = at("s2")});
		CHECK(other.decision == fs_write_decision::create_if_absent);
		// A target with no owner cannot be recorded, so it stays unseen.
		co_await observe(rt, at(""), true, "v1");
		auto ownerless = co_await waterfall(rt, araya::fs::write_intent_key, fs_write_intent{.target = at("")});
		CHECK(ownerless.decision == fs_write_decision::create_if_absent);
		co_return;
	});
}
