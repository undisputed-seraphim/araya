#include <catch2/catch_test_macros.hpp>

#include "coro_util.hpp"

#include "araya/events.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

inline constexpr araya::event_key<std::string, araya::dispatch_mode::serial> serial_key{"example.ingress.serial", 1};

inline constexpr araya::event_key<std::string, araya::dispatch_mode::emit> emit_key{"example.ingress.emit", 1};

inline constexpr araya::event_key<std::string, araya::dispatch_mode::waterfall> waterfall_key{
	"example.ingress.waterfall",
	1};

inline constexpr araya::event_key<std::string, araya::dispatch_mode::bail> bail_key{"example.ingress.bail", 1};

// Runs the io_context on a background thread so the test body can act as a
// foreign producer. The work guard keeps io.run() alive between postings.
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

template <class Pred>
bool wait_for(Pred pred) {
	for (int i = 0; i < 2000000; ++i) {
		if (pred())
			return true;
		std::this_thread::yield();
	}
	return false;
}

std::shared_ptr<araya::event_bus> make_bus(boost::asio::io_context& io) {
	return std::make_shared<araya::event_bus>(boost::asio::make_strand(io.get_executor()));
}

} // namespace

TEST_CASE("submit_nowait from a foreign thread delivers on the control strand") {
	driver d;
	auto bus = make_bus(d.io);

	std::atomic<int> delivered{0};
	std::atomic<bool> on_strand{false};
	std::atomic<bool> registered{false};
	boost::asio::post(bus->executor(), [&] {
		bus->add_listener(
			emit_key,
			[&](std::string const&) {
				on_strand.store(bus->on_control_strand());
				delivered.fetch_add(1);
			},
			0);
		registered.store(true);
	});
	CHECK(wait_for([&] { return registered.load(); }));

	std::thread producer([&] { bus->submit_nowait(emit_key, std::string("x")); });
	producer.join();

	CHECK(wait_for([&] { return delivered.load() == 1; }));
	CHECK(on_strand.load());
	d.stop();
}

TEST_CASE("submit from a foreign executor returns the waterfall result") {
	driver d;
	auto bus = make_bus(d.io);

	std::atomic<bool> registered{false};
	boost::asio::post(bus->executor(), [&] {
		bus->add_listener(
			waterfall_key,
			[](std::string const& m, araya::waterfall_continuation<std::string> next) -> araya::task<std::string> {
				co_return co_await next(m + "!");
			},
			0);
		registered.store(true);
	});
	CHECK(wait_for([&] { return registered.load(); }));

	std::string result;
	std::atomic<bool> done{false};
	// Spawn on the raw io executor, not the control strand, so the submit
	// must hop onto the strand itself.
	boost::asio::co_spawn(
		d.io.get_executor(),
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			result = co_await bus->submit(waterfall_key, std::string("x"));
			done.store(true, std::memory_order_release);
		}),
		boost::asio::detached);

	CHECK(wait_for([&] { return done.load(std::memory_order_acquire); }));
	CHECK(result == "x!");
	d.stop();
}

TEST_CASE("submit from a foreign executor returns the bail flag") {
	driver d;
	auto bus = make_bus(d.io);

	std::atomic<bool> registered{false};
	boost::asio::post(bus->executor(), [&] {
		bus->add_listener(bail_key, [](std::string const&) -> bool { return true; }, 0);
		registered.store(true);
	});
	CHECK(wait_for([&] { return registered.load(); }));

	bool bailed = false;
	std::atomic<bool> done{false};
	boost::asio::co_spawn(
		d.io.get_executor(),
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			bailed = co_await bus->submit(bail_key, std::string("x"));
			done.store(true, std::memory_order_release);
		}),
		boost::asio::detached);

	CHECK(wait_for([&] { return done.load(std::memory_order_acquire); }));
	CHECK(bailed);
	d.stop();
}

TEST_CASE("concurrent submit_nowait producers are linearized on the strand") {
	driver d;
	auto bus = make_bus(d.io);

	constexpr int producers = 4;
	constexpr int per_producer = 50;

	std::atomic<int> delivered{0};
	std::atomic<bool> all_on_strand{true};
	std::atomic<bool> registered{false};
	std::mutex mu;
	std::vector<std::string> seen;
	boost::asio::post(bus->executor(), [&] {
		bus->add_listener(
			serial_key,
			[&](std::string const& m) {
				if (!bus->on_control_strand())
					all_on_strand.store(false);
				{
					std::lock_guard lock{mu};
					seen.push_back(m);
				}
				delivered.fetch_add(1);
			},
			0);
		registered.store(true);
	});
	CHECK(wait_for([&] { return registered.load(); }));

	std::vector<std::thread> threads;
	threads.reserve(producers);
	for (int p = 0; p < producers; ++p) {
		threads.emplace_back([&, p] {
			for (int i = 0; i < per_producer; ++i)
				bus->submit_nowait(serial_key, "p" + std::to_string(p) + "-" + std::to_string(i));
		});
	}
	for (auto& t : threads)
		t.join();

	CHECK(wait_for([&] { return delivered.load() == producers * per_producer; }));
	CHECK(all_on_strand.load());

	std::lock_guard lock{mu};
	for (int p = 0; p < producers; ++p) {
		std::string prefix = "p" + std::to_string(p) + "-";
		int next = 0;
		for (auto const& m : seen) {
			if (m.rfind(prefix, 0) == 0)
				CHECK(std::stoi(m.substr(prefix.size())) == next++);
		}
		CHECK(next == per_producer);
	}
	d.stop();
}

TEST_CASE("strand-only operations still throw off-strand") {
	driver d;
	auto bus = make_bus(d.io);

	// emit dispatch is synchronous, so its strand check runs on the caller.
	CHECK_THROWS_AS(bus->dispatch(emit_key, std::string("x")), std::logic_error);
	CHECK_THROWS_AS(bus->add_listener(serial_key, [](std::string const&) {}, 0), std::logic_error);
	CHECK_THROWS_AS(bus->listener_count(serial_key.id), std::logic_error);
	d.stop();
}

TEST_CASE("submit keeps the dispatch scope alive until the dispatch runs") {
	driver d;
	auto bus = make_bus(d.io);

	std::shared_ptr<araya::context> scope = araya::context::root();
	std::weak_ptr<araya::context> weak = scope;
	std::atomic<bool> alive_during{false};
	std::atomic<int> delivered{0};
	std::atomic<bool> registered{false};
	boost::asio::post(bus->executor(), [&] {
		bus->add_listener(
			serial_key,
			[&](std::string const&) {
				alive_during.store(!weak.expired());
				delivered.fetch_add(1);
			},
			0);
		registered.store(true);
	});
	CHECK(wait_for([&] { return registered.load(); }));

	// Submit from this (foreign) thread, then drop the caller's reference:
	// the queued submission is now the only owner of the dispatch scope.
	bus->submit_nowait(serial_key, std::string("x"), scope);
	scope.reset();

	CHECK(wait_for([&] { return delivered.load() == 1; }));
	CHECK(alive_during.load());
	CHECK(wait_for([&] { return weak.expired(); }));
	d.stop();
}

TEST_CASE("submit_nowait reports a dispatch failure to the diagnostic sink") {
	driver d;
	auto bus = make_bus(d.io);

	std::atomic<bool> reported{false};
	std::atomic<bool> registered{false};
	boost::asio::post(bus->executor(), [&] {
		bus->set_diagnostic_sink([&](std::exception_ptr) { reported.store(true); });
		bus->add_listener(serial_key, [](std::string const&) { throw std::runtime_error("boom"); }, 0);
		registered.store(true);
	});
	CHECK(wait_for([&] { return registered.load(); }));

	bus->submit_nowait(serial_key, std::string("x"));
	CHECK(wait_for([&] { return reported.load(); }));
	d.stop();
}
