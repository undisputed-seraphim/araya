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

inline constexpr araya::event_key<std::string, araya::dispatch_mode::serial>
    serial_key{"example.serial", 1};

inline constexpr araya::event_key<std::string,
                                    araya::dispatch_mode::parallel>
    parallel_key{"example.parallel", 1};

inline constexpr araya::event_key<std::string, araya::dispatch_mode::emit>
    emit_key{"example.emit", 1};

inline constexpr araya::event_key<std::string,
                                    araya::dispatch_mode::waterfall>
    waterfall_key{"example.waterfall", 1};

struct fixture {
    boost::asio::io_context io;
    boost::asio::strand<boost::asio::any_io_executor> strand =
        boost::asio::make_strand(io.get_executor());
    std::shared_ptr<araya::event_bus> bus = std::make_shared<araya::event_bus>(strand);
    std::shared_ptr<araya::context> root = araya::context::root();
    std::shared_ptr<araya::activation> act =
        std::make_shared<araya::activation>(root);
    araya::plugin_context ctx{act};

    fixture() { act->bus = bus; }
};

}  // namespace

TEST_CASE("serial dispatch awaits listeners in registration order") {
    fixture fx;
    std::vector<std::string> order;

    boost::asio::co_spawn(
        fx.strand,
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            fx.ctx.on(serial_key, [&](std::string const& m) {
                order.push_back("sync:" + m);
            });
            fx.ctx.on(serial_key,
                      [&](std::string const& m) -> araya::task<void> {
                          order.push_back("async:" + m);
                          co_return;
                      });
            fx.ctx.on(serial_key, [&](std::string const& m) {
                order.push_back("last:" + m);
            });

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
            fx.ctx.on(serial_key, [&](std::string const& m) {
                order.push_back("first");
            });
            fx.ctx.on(serial_key, [&](std::string const& m) {
                order.push_back("boom");
                throw std::runtime_error("listener failure");
            });
            fx.ctx.on(serial_key, [&](std::string const& m) {
                order.push_back("never");
            });

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
                fx.ctx.on(parallel_key,
                          [&, i](std::string const& m) -> araya::task<void> {
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

    fx.bus->set_diagnostic_sink(
        [&](std::exception_ptr ep) { reported = ep; });

    boost::asio::co_spawn(
        fx.strand,
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            fx.ctx.on(emit_key, [&](std::string const& m) {
                ran.push_back("ok:" + m);
            });
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
    CHECK(ran == std::vector<std::string>{"ok:e", "bad:e",
                                          "dispatch-returned"});
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
                [&](std::string const& m,
                    araya::waterfall_continuation<std::string> next)
                    -> araya::task<std::string> {
                    order.push_back("first:" + m);
                    co_return co_await next(m + "+1");
                });
            fx.ctx.on(
                waterfall_key,
                [&](std::string const& m,
                    araya::waterfall_continuation<std::string> next)
                    -> araya::task<std::string> {
                    order.push_back("second:" + m);
                    if (m == "x+1") {
                        co_return "short-circuited";
                    }
                    co_return co_await next(m + "+2");
                });
            fx.ctx.on(
                waterfall_key,
                [&](std::string const& m,
                    araya::waterfall_continuation<std::string>)
                    -> araya::task<std::string> {
                    order.push_back("never");
                    co_return m;
                });

            result = co_await fx.bus->dispatch(waterfall_key,
                                               std::string("x"));
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
                [&](std::string const& m,
                    araya::waterfall_continuation<std::string> next)
                    -> araya::task<std::string> {
                    co_return co_await next(m + "+");
                });

            result = co_await fx.bus->dispatch(waterfall_key,
                                               std::string("x"));
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
                [&](std::string const& m,
                    araya::waterfall_continuation<std::string>)
                    -> araya::task<std::string> {
                    order.push_back("boom");
                    throw std::runtime_error("waterfall failure");
                });
            fx.ctx.on(
                waterfall_key,
                [&](std::string const& m,
                    araya::waterfall_continuation<std::string>)
                    -> araya::task<std::string> {
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
            auto reg = fx.ctx.on(serial_key, [&](std::string const& m) {
                order.push_back(m);
            });
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
                fx.ctx.on(serial_key, [&](std::string const& m2) {
                    order.push_back("late");
                });
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
    araya::event_key<std::string, araya::dispatch_mode::parallel>
        serial_id_parallel_mode{serial_key.id};

    boost::asio::co_spawn(
        fx.strand,
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            fx.ctx.on(serial_key, [](std::string const&) {});
            CHECK_THROWS_AS(
                fx.ctx.on(serial_id_parallel_mode, [](std::string const&) {}),
                std::logic_error);
            CHECK_THROWS_AS(
                co_await fx.bus->dispatch(serial_id_parallel_mode,
                                          std::string("x")),
                std::logic_error);
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
            fx.bus->set_diagnostic_sink(
                [&](std::exception_ptr ep) { first_sink_report = ep; });
            fx.ctx.on(emit_key, [&](std::string const& m) -> araya::task<void> {
                auto timer = boost::asio::steady_timer(
                    co_await boost::asio::this_coro::executor, 20ms);
                co_await timer.async_wait(boost::asio::use_awaitable);
                ran.push_back("bad:" + m);
                throw std::runtime_error("late failure");
            });
            fx.bus->dispatch(emit_key, std::string("e"));

            // replace the sink while the listener is still in flight
            fx.bus->set_diagnostic_sink(
                [&](std::exception_ptr ep) { second_sink_report = ep; });
            co_return;
        }),
        boost::asio::detached);

    fx.io.run();
    CHECK(ran == std::vector<std::string>{"bad:e"});
    CHECK(first_sink_report == nullptr);
    CHECK(second_sink_report != nullptr);
}
