#include <catch2/catch_test_macros.hpp>

#include "araya/jobs/jobs.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"

#include "support/plugin_harness.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace araya::jobs;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec jobs_spec(std::size_t max_concurrent = 10) {
		return spec(
			&araya::jobs::plugin_descriptor(),
			{
				{"max_concurrent_jobs_per_owner", std::to_string(max_concurrent)},
			});
	}
};

struct rig {
	std::shared_ptr<jobs_service> jobs;
	std::shared_ptr<araya::session::session_store> store;

	araya::task<void> mount(araya::runtime& rt, harness& h, std::size_t max_concurrent = 10) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.jobs_spec(max_concurrent));
		co_await rt.wait_idle();
		auto root = rt.root_context();
		store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		jobs = root.require<jobs_service>(jobs_key).shared();
		jobs->attach_controller(root, "test");
		store->create(root, araya::session::session_id{"s1"}, {});
	}
};

job_start owned_start(
	std::string label,
	std::function<void(job_outcome)>* settle_out,
	std::string* cancel_reason = nullptr,
	std::shared_ptr<std::string> buffer = nullptr,
	std::shared_ptr<std::size_t> cursor = nullptr) {
	return job_start{
		.kind = "bash",
		.label = std::move(label),
		.owner_session = std::string{"s1"},
		.output_limit_bytes = std::size_t{4096},
		.run =
			[settle_out, cancel_reason, buffer, cursor](std::function<void(job_outcome)> settle) {
				*settle_out = std::move(settle);
				job_handle handle;
				if (cancel_reason)
					handle.cancel = [cancel_reason](std::string const& reason) { *cancel_reason = reason; };
				if (buffer && cursor)
					handle.read_output = [buffer, cursor] {
						auto text = buffer->substr(*cursor);
						*cursor = buffer->size();
						return text;
					};
				return handle;
			},
	};
}

} // namespace

TEST_CASE("start issues ids, lists, and reads a consuming cursor") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		std::function<void(job_outcome)> settle;
		auto buffer = std::make_shared<std::string>("hello ");
		auto cursor = std::make_shared<std::size_t>(0);
		auto id = r.jobs->start(owned_start("echo hi", &settle, nullptr, buffer, cursor));
		CHECK(id == "bash-1");

		auto list = r.jobs->list(std::string{"s1"});
		REQUIRE(list.size() == 1);
		CHECK(list[0].id == "bash-1");
		CHECK(list[0].kind == "bash");
		CHECK(list[0].label == "echo hi");
		CHECK(list[0].status == job_status::running);
		CHECK(list[0].owner_session.has_value());
		CHECK_FALSE(list[0].reported);

		CHECK(r.jobs->list(std::string{"other"}).empty());
		CHECK(r.jobs->list(std::nullopt).empty());

		*buffer += "world";
		auto first = r.jobs->read(id, std::string{"s1"});
		CHECK(first.text == "hello world");
		auto second = r.jobs->read(id, std::string{"s1"});
		CHECK(second.text.empty());

		settle(job_outcome{job_status::completed, "exit code: 0", {}});
		auto snapshot = r.jobs->get(id, std::string{"s1"});
		CHECK(snapshot.status == job_status::completed);
		CHECK(snapshot.detail == std::optional<std::string>{"exit code: 0"});
		CHECK(snapshot.finished_at_ms.has_value());
	});
}

TEST_CASE("final-output jobs return their result only after settlement") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		std::function<void(job_outcome)> settle;
		auto id = r.jobs->start(owned_start("final", &settle));

		CHECK(r.jobs->read(id, std::string{"s1"}).text.empty());
		settle(job_outcome{job_status::completed, {}, "the result"});
		CHECK(r.jobs->read(id, std::string{"s1"}).text == "the result");
		// Final output is idempotent, not consumed.
		CHECK(r.jobs->read(id, std::string{"s1"}).text == "the result");
	});
}

TEST_CASE("kill requests cancellation once and reports already-finished after settlement") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		std::function<void(job_outcome)> settle;
		std::string reason;
		auto id = r.jobs->start(owned_start("kill me", &settle, &reason));

		CHECK(r.jobs->kill(id, std::string{"s1"}, "no longer needed") == jobs_service::kill_result::requested);
		CHECK(reason == "no longer needed");
		CHECK(r.jobs->get(id, std::string{"s1"}).status == job_status::stopping);
		CHECK(r.jobs->get(id, std::string{"s1"}).reported);

		settle(job_outcome{job_status::killed, "signal: 9", {}});
		CHECK(r.jobs->kill(id, std::string{"s1"}, "again") == jobs_service::kill_result::already_finished);
	});
}

TEST_CASE("settlement is first-wins") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		std::function<void(job_outcome)> settle;
		auto id = r.jobs->start(owned_start("once", &settle));
		settle(job_outcome{job_status::completed, "first", {}});
		settle(job_outcome{job_status::failed, "second", {}});
		CHECK(r.jobs->get(id, std::string{"s1"}).detail == std::optional<std::string>{"first"});
	});
}

TEST_CASE("wait returns at settlement and on timeout") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		std::function<void(job_outcome)> settle;
		auto id = r.jobs->start(owned_start("waited", &settle));

		auto worker = [&h, &settle]() -> araya::task<void> {
			boost::asio::steady_timer timer(h.io.get_executor());
			timer.expires_after(std::chrono::milliseconds(2));
			co_await timer.async_wait(boost::asio::use_awaitable);
			settle(job_outcome{job_status::completed, "exit code: 0", {}});
		};
		boost::asio::co_spawn(h.io.get_executor(), worker(), boost::asio::detached);

		auto snapshot = co_await r.jobs->wait(id, 5000, std::string{"s1"}, {});
		CHECK(snapshot.status == job_status::completed);
		CHECK(snapshot.reported);

		std::function<void(job_outcome)> never;
		auto slow = r.jobs->start(owned_start("slow", &never));
		auto timed_out = co_await r.jobs->wait(slow, 2, std::string{"s1"}, {});
		CHECK(timed_out.status == job_status::running);
		CHECK_FALSE(timed_out.reported);
	});
}

TEST_CASE("owner fencing rejects foreign callers") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		std::function<void(job_outcome)> settle;
		auto id = r.jobs->start(owned_start("private", &settle));

		CHECK_THROWS_AS(r.jobs->get(id, std::string{"other"}), std::runtime_error);
		CHECK_THROWS_AS(r.jobs->read(id, std::string{"other"}), std::runtime_error);
		CHECK_THROWS_AS(r.jobs->kill(id, std::string{"other"}, "no"), std::runtime_error);
		CHECK_THROWS_AS(r.jobs->get(id, std::nullopt), std::runtime_error);
		CHECK_THROWS_AS(r.jobs->get("bash-999", std::string{"s1"}), std::runtime_error);
	});
}

TEST_CASE("the per-owner concurrency cap admits only the configured number") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h, /*max_concurrent=*/2);

		std::function<void(job_outcome)> a;
		std::function<void(job_outcome)> b;
		std::function<void(job_outcome)> c;
		r.jobs->start(owned_start("one", &a));
		r.jobs->start(owned_start("two", &b));
		CHECK_THROWS_AS(r.jobs->start(owned_start("three", &c)), std::runtime_error);
	});
}

TEST_CASE("session disposal cancels and removes owned jobs") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		std::function<void(job_outcome)> settle;
		std::string reason;
		auto id = r.jobs->start(owned_start("owned", &settle, &reason));
		CHECK(reason.empty());

		CHECK(r.store->dispose(araya::session::session_id{"s1"}));
		CHECK(reason == "owner disposed");
		CHECK(r.jobs->list(std::string{"s1"}).empty());
		CHECK_THROWS_AS(r.jobs->get(id, std::string{"s1"}), std::runtime_error);
	});
}

TEST_CASE("completion listeners fire after settlement") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		rig r;
		co_await r.mount(rt, h);

		auto root = rt.root_context();
		std::optional<job_snapshot> seen;
		std::optional<std::string> seen_owner;
		r.jobs->on_job_done(root, [&](job_snapshot const& snapshot, std::optional<std::string> const& owner) {
			seen = snapshot;
			seen_owner = owner;
		});

		std::function<void(job_outcome)> settle;
		auto id = r.jobs->start(owned_start("notify", &settle));
		CHECK_FALSE(seen.has_value());

		settle(job_outcome{job_status::completed, "exit code: 0", {}});
		REQUIRE(seen.has_value());
		CHECK(seen->id == id);
		CHECK(seen->status == job_status::completed);
		REQUIRE(seen_owner.has_value());
		CHECK(*seen_owner == "s1");
	});
}

TEST_CASE("start refuses without an attached controller") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.jobs_spec());
		co_await rt.wait_idle();
		auto root = rt.root_context();
		auto jobs = root.require<jobs_service>(jobs_key).shared();

		std::function<void(job_outcome)> settle;
		CHECK_THROWS_AS(jobs->start(owned_start("gated", &settle)), std::runtime_error);

		jobs->attach_controller(root, "test");
		CHECK_NOTHROW(jobs->start(owned_start("allowed", &settle)));
	});
}
