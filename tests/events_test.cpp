#include <catch2/catch_test_macros.hpp>

#include "coro_util.hpp"

#include "araya/events.hpp"
#include "araya/plugin_context.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

inline constexpr araya::event_key<std::string, araya::dispatch_mode::serial> serial_key{"example.serial", 1};

inline constexpr araya::event_key<std::string, araya::dispatch_mode::parallel> parallel_key{"example.parallel", 1};

inline constexpr araya::event_key<std::string, araya::dispatch_mode::emit> emit_key{"example.emit", 1};

inline constexpr araya::event_key<std::string, araya::dispatch_mode::waterfall> waterfall_key{"example.waterfall", 1};

inline constexpr araya::event_key<std::string, araya::dispatch_mode::bail> bail_key{"example.bail", 1};

boost::asio::awaitable<void> dummy_raw(void const*) { co_return; }

// Throws from a plain (non-coroutine) frame: the test coroutine only
// unwinds through the call, mirroring the listener-throw pattern.
void register_raw_on_bail(araya::event_bus& bus) {
	bus.add_raw_listener(araya::service_id{"example.bail", 1}, araya::dispatch_mode::bail, dummy_raw, 1);
}

struct fixture {
	boost::asio::io_context io;
	boost::asio::strand<boost::asio::any_io_executor> strand = boost::asio::make_strand(io.get_executor());
	std::shared_ptr<araya::event_bus> bus = std::make_shared<araya::event_bus>(strand);
	std::shared_ptr<araya::context> root = araya::context::root();
	std::shared_ptr<araya::activation> act = std::make_shared<araya::activation>(root);
	araya::plugin_context ctx{act};

	fixture() { act->bus = bus; }
};

} // namespace

TEST_CASE("serial dispatch awaits listeners in registration order") {
	fixture fx;
	std::vector<std::string> order;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(serial_key, [&](std::string const& m) { order.push_back("sync:" + m); });
			fx.ctx.on(serial_key, [&](std::string const& m) -> araya::task<void> {
				order.push_back("async:" + m);
				co_return;
			});
			fx.ctx.on(serial_key, [&](std::string const& m) { order.push_back("last:" + m); });

			co_await fx.bus->dispatch(serial_key, std::string("x"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(order == std::vector<std::string>{"sync:x", "async:x", "last:x"});
}

TEST_CASE("serial dispatch stops at the first failure") {
	fixture fx;
	std::vector<std::string> order;
	bool threw = false;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(serial_key, [&](std::string const& m) { order.push_back("first"); });
			fx.ctx.on(serial_key, [&](std::string const& m) {
				order.push_back("boom");
				throw std::runtime_error("listener failure");
			});
			fx.ctx.on(serial_key, [&](std::string const& m) { order.push_back("never"); });

			try {
				co_await fx.bus->dispatch(serial_key, std::string("x"));
			} catch (std::runtime_error const&) {
				threw = true;
			}
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(threw);
	CHECK(order == std::vector<std::string>{"first", "boom"});
}

TEST_CASE("parallel dispatch runs all listeners and rethrows after") {
	fixture fx;
	std::vector<std::string> ran;
	bool threw = false;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			for (int i = 0; i < 3; ++i) {
				fx.ctx.on(parallel_key, [&, i](std::string const& m) -> araya::task<void> {
					ran.push_back(m + std::to_string(i));
					co_return;
				});
			}
			fx.ctx.on(parallel_key, [&](std::string const& m) {
				ran.push_back("boom");
				throw std::runtime_error("parallel failure");
			});

			try {
				co_await fx.bus->dispatch(parallel_key, std::string("p"));
			} catch (std::runtime_error const&) {
				threw = true;
			}
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(threw);
	CHECK(ran.size() == 4);
	CHECK(std::find(ran.begin(), ran.end(), "boom") != ran.end());
}

TEST_CASE("emit dispatches without awaiting and reports failures") {
	fixture fx;
	std::vector<std::string> ran;
	std::exception_ptr reported;

	fx.bus->set_diagnostic_sink([&](std::exception_ptr ep) { reported = ep; });

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(emit_key, [&](std::string const& m) { ran.push_back("ok:" + m); });
			fx.ctx.on(emit_key, [&](std::string const& m) {
				ran.push_back("bad:" + m);
				throw std::runtime_error("emit failure");
			});

			fx.bus->dispatch(emit_key, std::string("e"));
			ran.push_back("dispatch-returned");
			co_return;
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(ran == std::vector<std::string>{"ok:e", "bad:e", "dispatch-returned"});
	CHECK(reported != nullptr);
}

TEST_CASE("waterfall delegates, transforms, and short-circuits") {
	fixture fx;
	std::vector<std::string> order;
	std::string result;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(
				waterfall_key,
				[&](std::string const& m, araya::waterfall_continuation<std::string> next) -> araya::task<std::string> {
					order.push_back("first:" + m);
					co_return co_await next(m + "+1");
				});
			fx.ctx.on(
				waterfall_key,
				[&](std::string const& m, araya::waterfall_continuation<std::string> next) -> araya::task<std::string> {
					order.push_back("second:" + m);
					if (m == "x+1") {
						co_return "short-circuited";
					}
					co_return co_await next(m + "+2");
				});
			fx.ctx.on(
				waterfall_key,
				[&](std::string const& m, araya::waterfall_continuation<std::string>) -> araya::task<std::string> {
					order.push_back("never");
					co_return m;
				});

			result = co_await fx.bus->dispatch(waterfall_key, std::string("x"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(result == "short-circuited");
	CHECK(order == std::vector<std::string>{"first:x", "second:x+1"});
}

TEST_CASE("waterfall passes through when no listener short-circuits") {
	fixture fx;
	std::string result;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(
				waterfall_key,
				[&](std::string const& m, araya::waterfall_continuation<std::string> next) -> araya::task<std::string> {
					co_return co_await next(m + "+");
				});

			result = co_await fx.bus->dispatch(waterfall_key, std::string("x"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(result == "x+");
}

TEST_CASE("waterfall exception terminates the chain") {
	fixture fx;
	std::vector<std::string> order;
	bool threw = false;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(
				waterfall_key,
				[&](std::string const& m, araya::waterfall_continuation<std::string>) -> araya::task<std::string> {
					order.push_back("boom");
					throw std::runtime_error("waterfall failure");
				});
			fx.ctx.on(
				waterfall_key,
				[&](std::string const& m, araya::waterfall_continuation<std::string>) -> araya::task<std::string> {
					order.push_back("never");
					co_return m;
				});

			try {
				co_await fx.bus->dispatch(waterfall_key, std::string("x"));
			} catch (std::runtime_error const&) {
				threw = true;
			}
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(threw);
	CHECK(order == std::vector<std::string>{"boom"});
}

TEST_CASE("listener registrations are owned effects") {
	fixture fx;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(serial_key, [](std::string const&) {});
			CHECK(fx.bus->listener_count(serial_key.id) == 1);
			fx.act->teardown();
			CHECK(fx.bus->listener_count(serial_key.id) == 0);
			co_await fx.bus->dispatch(serial_key, std::string("x"));
		}),
		boost::asio::detached);

	fx.io.run();
}

TEST_CASE("early release removes a listener immediately") {
	fixture fx;
	std::vector<std::string> order;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			auto reg = fx.ctx.on(serial_key, [&](std::string const& m) { order.push_back(m); });
			reg.release();
			CHECK(fx.bus->listener_count(serial_key.id) == 0);
			co_await fx.bus->dispatch(serial_key, std::string("x"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(order.empty());
}

TEST_CASE("listener added during dispatch affects only later dispatches") {
	fixture fx;
	std::vector<std::string> order;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(serial_key, [&](std::string const& m) -> araya::task<void> {
				order.push_back("first");
				fx.ctx.on(serial_key, [&](std::string const& m2) { order.push_back("late"); });
				co_return;
			});

			co_await fx.bus->dispatch(serial_key, std::string("1"));
			co_await fx.bus->dispatch(serial_key, std::string("2"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(order == std::vector<std::string>{"first", "first", "late"});
}

TEST_CASE("conflicting mode registration and dispatch are rejected") {
	fixture fx;
	araya::event_key<std::string, araya::dispatch_mode::parallel> serial_id_parallel_mode{serial_key.id};

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(serial_key, [](std::string const&) {});
			CHECK_THROWS_AS(fx.ctx.on(serial_id_parallel_mode, [](std::string const&) {}), std::logic_error);
			CHECK_THROWS_AS(co_await fx.bus->dispatch(serial_id_parallel_mode, std::string("x")), std::logic_error);
		}),
		boost::asio::detached);

	fx.io.run();
}

TEST_CASE("bus operations require the control strand") {
	fixture fx;
	CHECK_THROWS_AS(fx.bus->listener_count(serial_key.id), std::logic_error);
}

TEST_CASE("in-flight emit observes the current sink without dangling") {
	fixture fx;
	std::vector<std::string> ran;
	std::exception_ptr first_sink_report;
	std::exception_ptr second_sink_report;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.bus->set_diagnostic_sink([&](std::exception_ptr ep) { first_sink_report = ep; });
			fx.ctx.on(emit_key, [&](std::string const& m) -> araya::task<void> {
				auto timer = boost::asio::steady_timer(co_await boost::asio::this_coro::executor, 20ms);
				co_await timer.async_wait(boost::asio::use_awaitable);
				ran.push_back("bad:" + m);
				throw std::runtime_error("late failure");
			});
			fx.bus->dispatch(emit_key, std::string("e"));

			// replace the sink while the listener is still in flight
			fx.bus->set_diagnostic_sink([&](std::exception_ptr ep) { second_sink_report = ep; });
			co_return;
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(ran == std::vector<std::string>{"bad:e"});
	CHECK(first_sink_report == nullptr);
	CHECK(second_sink_report != nullptr);
}

TEST_CASE("bail dispatch stops at the first listener returning true") {
	fixture fx;
	std::vector<std::string> order;
	bool bailed = false;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(bail_key, [&](std::string const& m) -> bool {
				order.push_back("false:" + m);
				return false;
			});
			fx.ctx.on(bail_key, [&](std::string const& m) -> bool {
				order.push_back("true:" + m);
				return true;
			});
			fx.ctx.on(bail_key, [&](std::string const& m) -> bool {
				order.push_back("never:" + m);
				return false;
			});

			bailed = co_await fx.bus->dispatch(bail_key, std::string("x"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(bailed);
	CHECK(order == std::vector<std::string>{"false:x", "true:x"});
}

TEST_CASE("bail dispatch reports false when nobody bails") {
	fixture fx;
	std::vector<std::string> ran;
	bool bailed = true;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(bail_key, [&](std::string const& m) {
				ran.push_back("sync:" + m);
				return false;
			});
			fx.ctx.on(bail_key, [&](std::string const& m) -> araya::task<bool> {
				ran.push_back("async:" + m);
				co_return false;
			});

			bailed = co_await fx.bus->dispatch(bail_key, std::string("x"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(!bailed);
	CHECK(ran == std::vector<std::string>{"sync:x", "async:x"});
}

TEST_CASE("bail dispatch stops at the first failure") {
	fixture fx;
	std::vector<std::string> order;
	bool threw = false;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(bail_key, [&](std::string const& m) -> bool {
				order.push_back("boom:" + m);
				throw std::runtime_error("bail listener failure");
			});
			fx.ctx.on(bail_key, [&](std::string const& m) -> bool {
				order.push_back("never:" + m);
				return false;
			});

			try {
				(void)co_await fx.bus->dispatch(bail_key, std::string("x"));
			} catch (std::runtime_error const&) {
				threw = true;
			}
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(threw);
	CHECK(order == std::vector<std::string>{"boom:x"});
}

TEST_CASE("raw listeners are rejected on bail-mode events") {
	fixture fx;
	std::exception_ptr caught;

	// The rejection surfaces through the co_spawn completion handler:
	// try/catch around a plain call inside a coroutine frame is
	// miscompiled by GCC 14 at -O0 (ud2 in the try region).
	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(bail_key, [](std::string const&) { return true; });
			register_raw_on_bail(*fx.bus);
		}),
		[&caught](std::exception_ptr ep) { caught = ep; });

	fx.io.run();
	REQUIRE(caught != nullptr);
	bool is_logic = false;
	try {
		std::rethrow_exception(caught);
	} catch (std::logic_error const&) {
		is_logic = true;
	}
	CHECK(is_logic);
}

TEST_CASE("once listeners run at most once and self-remove") {
	fixture fx;
	int calls = 0;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(serial_key, [&](std::string const&) { ++calls; }, araya::listener_options{.once = true});
			CHECK(fx.bus->listener_count(serial_key.id) == 1);

			co_await fx.bus->dispatch(serial_key, std::string("x"));
			CHECK(fx.bus->listener_count(serial_key.id) == 0);

			co_await fx.bus->dispatch(serial_key, std::string("y"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(calls == 1);
}

TEST_CASE("once listeners self-remove even when they throw") {
	fixture fx;
	int calls = 0;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(
				serial_key,
				[&](std::string const&) {
					++calls;
					throw std::runtime_error("once failure");
				},
				araya::listener_options{.once = true});

			try {
				co_await fx.bus->dispatch(serial_key, std::string("x"));
			} catch (std::runtime_error const&) {
			}
			CHECK(fx.bus->listener_count(serial_key.id) == 0);

			co_await fx.bus->dispatch(serial_key, std::string("y"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(calls == 1);
}

TEST_CASE("prepend listeners run ahead of existing ones") {
	fixture fx;
	std::vector<std::string> order;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(serial_key, [&](std::string const&) { order.push_back("first"); });
			fx.ctx.on(
				serial_key,
				[&](std::string const&) { order.push_back("second"); },
				araya::listener_options{.prepend = true});

			co_await fx.bus->dispatch(serial_key, std::string("x"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(order == std::vector<std::string>{"second", "first"});
}

TEST_CASE("scoped listeners deliver only to matching-scope dispatches") {
	fixture fx;
	auto iso = fx.root->make_child();
	iso->isolate(serial_key.id, "r");
	std::vector<std::string> got;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(
				serial_key,
				[&](std::string const& m) { got.push_back("scoped:" + m); },
				araya::listener_options{.scope = iso.get()});

			// Root scope has no realm label for the key: mismatch.
			co_await fx.bus->dispatch(serial_key, std::string("a"), fx.root.get());
			// The isolated scope carries the "r" label: match.
			co_await fx.bus->dispatch(serial_key, std::string("b"), iso.get());
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(got == std::vector<std::string>{"scoped:b"});
}

TEST_CASE("scope-less dispatch broadcasts to scoped and unscoped listeners") {
	fixture fx;
	auto iso = fx.root->make_child();
	iso->isolate(serial_key.id, "r");
	std::vector<std::string> got;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(serial_key, [&](std::string const& m) { got.push_back("unscoped:" + m); });
			fx.ctx.on(
				serial_key,
				[&](std::string const& m) { got.push_back("scoped:" + m); },
				araya::listener_options{.scope = iso.get()});

			co_await fx.bus->dispatch(serial_key, std::string("x"));
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(got == std::vector<std::string>{"unscoped:x", "scoped:x"});
}

TEST_CASE("unscoped listeners deliver even to foreign scopes") {
	fixture fx;
	auto iso = fx.root->make_child();
	iso->isolate(serial_key.id, "r");
	std::vector<std::string> got;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(serial_key, [&](std::string const& m) { got.push_back("unscoped:" + m); });

			// The dispatching scope matches nothing the listener declared;
			// an unscoped listener still receives.
			co_await fx.bus->dispatch(serial_key, std::string("x"), iso.get());
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(got == std::vector<std::string>{"unscoped:x"});
}

TEST_CASE("global scoped listeners ignore the dispatch scope") {
	fixture fx;
	auto iso = fx.root->make_child();
	iso->isolate(serial_key.id, "r");
	std::vector<std::string> got;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(
				serial_key,
				[&](std::string const& m) { got.push_back("global:" + m); },
				araya::listener_options{.global = true, .scope = iso.get()});

			co_await fx.bus->dispatch(serial_key, std::string("x"), fx.root.get());
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(got == std::vector<std::string>{"global:x"});
}

TEST_CASE("realm labels are inherited by descendant scopes") {
	fixture fx;
	auto iso = fx.root->make_child();
	iso->isolate(serial_key.id, "r");
	auto child = iso->make_child();
	std::vector<std::string> got;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(
				serial_key,
				[&](std::string const& m) { got.push_back("scoped:" + m); },
				araya::listener_options{.scope = iso.get()});

			// The child has no own tag; realm_for walks the chain to "r".
			co_await fx.bus->dispatch(serial_key, std::string("x"), child.get());
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(got == std::vector<std::string>{"scoped:x"});
}

TEST_CASE("scopes sharing a realm label join") {
	fixture fx;
	auto a = fx.root->make_child();
	auto b = fx.root->make_child();
	a->isolate(serial_key.id, "shared");
	b->isolate(serial_key.id, "shared");
	std::vector<std::string> got;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(
				serial_key,
				[&](std::string const& m) { got.push_back("joined:" + m); },
				araya::listener_options{.scope = a.get()});

			co_await fx.bus->dispatch(serial_key, std::string("x"), b.get());
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(got == std::vector<std::string>{"joined:x"});
}

TEST_CASE("scope filtering applies to parallel dispatch") {
	fixture fx;
	auto iso = fx.root->make_child();
	iso->isolate(parallel_key.id, "r");
	std::vector<std::string> got;

	boost::asio::co_spawn(
		fx.strand,
		araya_test::heap_coroutine([&]() -> araya::task<void> {
			fx.ctx.on(
				parallel_key,
				[&](std::string const& m) { got.push_back("scoped:" + m); },
				araya::listener_options{.scope = iso.get()});

			co_await fx.bus->dispatch(parallel_key, std::string("a"), fx.root.get());
			co_await fx.bus->dispatch(parallel_key, std::string("b"), iso.get());
		}),
		boost::asio::detached);

	fx.io.run();
	CHECK(got == std::vector<std::string>{"scoped:b"});
}
