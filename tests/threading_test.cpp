#include <catch2/catch_test_macros.hpp>

#include "araya/runtime.hpp"
#include "araya/task.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

namespace {

// The documented thread-safe handle surface: state(), error(), and
// cancel() may be called from any thread. These tests drive fibers on a
// background io thread while the main thread polls the handle - the TSan
// build turns this into a data-race proof for the cell publication
// (release/acquire on error+completed and the relaxed state read).
//
// The work guard keeps io.run() alive between postings; without it the
// context drains and returns whenever the queue is momentarily empty,
// silently dropping later posts.
struct driver {
	boost::asio::io_context io;
	using guard_t = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
	std::optional<guard_t> guard;
	std::thread thread;

	driver()
		: guard(boost::asio::make_work_guard(io))
		, thread([this] { io.run(); }) {}

	void stop() { guard.reset(); }

	~driver() {
		stop();
		thread.join();
	}
};

} // namespace

TEST_CASE("foreign-thread handle polling and cancel race-free") {
	driver d;
	araya::fiber_handle h;
	std::atomic<bool> started{false};
	boost::asio::post(d.io, [&] {
		h = araya::spawn(d.io.get_executor(), [&]() -> araya::task<void> {
			auto token = co_await araya::this_stop_token();
			while (!token.stop_requested()) {
				auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor, 5ms);
				co_await timer.async_wait(boost::asio::use_awaitable);
			}
		});
		// Publish the handle here, sequenced after the assignment: the
		// acquire below must observe h, and a store made inside the
		// fiber would not be ordered with respect to the handler's
		// assignment.
		started.store(true, std::memory_order_release);
	});
	while (!started.load(std::memory_order_acquire)) {
	}

	// Poll the handle from the test thread while the fiber runs.
	bool saw_loading = false;
	for (int i = 0; i < 1000; ++i) {
		if (h.state() == araya::fiber_state::loading)
			saw_loading = true;
		CHECK(h.error() == nullptr);
	}
	h.cancel();
	while (h.state() != araya::fiber_state::inactive) {
		std::this_thread::yield();
	}

	CHECK(saw_loading);
	CHECK(h.error() == nullptr);
	d.stop();
}

TEST_CASE("foreign-thread runtime queries race-free") {
	driver d;
	auto rt = std::make_shared<araya::runtime>(d.io.get_executor());
	araya::fiber_handle provider;
	std::atomic<bool> mounted{false};
	boost::asio::post(d.io, [&] {
		boost::asio::co_spawn(
			d.io.get_executor(),
			[&]() -> araya::task<void> {
				co_await rt->run_on_strand([&] {
					// mount_locked is strand-only; the polling below
					// exercises the documented thread-safe surface.
					provider = rt->mount_locked([] {
						struct p : araya::plugin {
							araya::task<void> apply(araya::plugin_context&) override { co_return; }
						};
						static constexpr std::span<araya::dependency_spec const> no_deps{};
						static constexpr std::span<araya::provision_spec const> no_provs{};
						static const araya::plugin_descriptor desc{
							"idle", no_deps, no_provs, [](araya::plugin_config const&) {
								return std::make_unique<p>();
							}};
						return araya::component_spec{
							std::shared_ptr<araya::plugin_descriptor>(
								const_cast<araya::plugin_descriptor*>(&desc), [](auto*) {}),
							{},
							nullptr,
							""};
					}());
					mounted.store(true, std::memory_order_release);
				});
			},
			boost::asio::detached);
	});

	while (!mounted.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	for (int i = 0; i < 1000; ++i) {
		(void)provider.state();
		CHECK(provider.error() == nullptr);
		CHECK_FALSE(rt->bus()->on_control_strand());
	}
	boost::asio::post(d.io, [&] { rt->retire_child(provider.id()); });
	while (provider.state() != araya::fiber_state::inactive) {
		std::this_thread::yield();
	}
	// The state flip and the record erasure are separate strand
	// handlers: run a barrier through the runtime strand so the erasure
	// has definitely happened before the runtime is destroyed - the
	// destructor drains its io_context, which must not be driven from
	// the driver thread at the same time.
	std::atomic<bool> drained{false};
	boost::asio::post(d.io, [&] {
		boost::asio::co_spawn(
			d.io.get_executor(),
			[&]() -> araya::task<void> {
				co_await rt->run_on_strand([&] { drained.store(true, std::memory_order_release); });
			},
			boost::asio::detached);
	});
	while (!drained.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	d.stop();
}
