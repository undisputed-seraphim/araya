#include <catch2/catch_test_macros.hpp>

#include "coro_util.hpp"

#include "araya/task.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <stdexcept>

using namespace std::chrono_literals;

namespace {

araya::task<void> wait_until_stopped(bool& seen) {
    auto token = co_await araya::this_stop_token();
    while (!token.stop_requested()) {
        auto timer = boost::asio::steady_timer(
            co_await boost::asio::this_coro::executor, 10ms);
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    seen = true;
}

}  // namespace

TEST_CASE("spawned task completes and fiber becomes active") {
    boost::asio::io_context io;
    int result = 0;
    auto h = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<int> {
            result = 42;
            co_return 42;
        }));

    CHECK(h.state() == araya::fiber_state::loading);
    io.run();

    CHECK(result == 42);
    CHECK(h.state() == araya::fiber_state::active);
}

TEST_CASE("exception marks fiber inactive") {
    boost::asio::io_context io;
    auto h = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            throw std::runtime_error("boom");
            co_return;
        }));

    io.run();
    CHECK(h.state() == araya::fiber_state::inactive);
}

TEST_CASE("spawned failure is readable from the handle") {
    boost::asio::io_context io;
    auto h = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            throw std::runtime_error("boom");
            co_return;
        }));

    io.run();
    auto ep = h.error();
    REQUIRE(ep != nullptr);
    try {
        std::rethrow_exception(ep);
    } catch (std::runtime_error const& e) {
        CHECK(std::string(e.what()) == "boom");
    }
}

TEST_CASE("spawned clean completion publishes a null error") {
    boost::asio::io_context io;
    auto h = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine(
            [&]() -> araya::task<void> { co_return; }));

    io.run();
    CHECK(h.state() == araya::fiber_state::active);
    CHECK(h.error() == nullptr);
}

TEST_CASE("handle error is null while the task still runs") {
    boost::asio::io_context io;
    auto h = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            auto timer = boost::asio::steady_timer(
                co_await boost::asio::this_coro::executor, 40ms);
            co_await timer.async_wait(boost::asio::use_awaitable);
        }));

    boost::asio::steady_timer check{io, 10ms};
    check.async_wait([&](boost::system::error_code) {
        CHECK(h.state() == araya::fiber_state::loading);
        CHECK(h.error() == nullptr);
    });

    io.run();
    CHECK(h.error() == nullptr);
}

TEST_CASE("cancel is observed cooperatively through this_stop_token") {
    boost::asio::io_context io;
    bool seen = false;
    auto h = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            co_await wait_until_stopped(seen);
        }));

    boost::asio::steady_timer cancel_timer{io, 50ms};
    cancel_timer.async_wait(
        [h](boost::system::error_code) { h.cancel(); });

    io.run();
    CHECK(seen);
    CHECK(h.state() == araya::fiber_state::inactive);
}

TEST_CASE("cancel does not abort an in-flight operation") {
    boost::asio::io_context io;
    bool timer_done = false;
    bool stop_seen = false;
    auto h = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            auto timer = boost::asio::steady_timer(
                co_await boost::asio::this_coro::executor, 60ms);
            co_await timer.async_wait(boost::asio::use_awaitable);
            timer_done = true;
            auto token = co_await araya::this_stop_token();
            stop_seen = token.stop_requested();
        }));

    auto start = std::chrono::steady_clock::now();
    boost::asio::steady_timer cancel_timer{io, 20ms};
    cancel_timer.async_wait(
        [h](boost::system::error_code) { h.cancel(); });

    io.run();
    CHECK(timer_done);
    CHECK(stop_seen);
    CHECK(std::chrono::steady_clock::now() - start >= 40ms);
    CHECK(h.state() == araya::fiber_state::inactive);
}

TEST_CASE("stop_requested resumes a suspended fiber") {
    boost::asio::io_context io;
    bool resumed = false;
    auto h = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            resumed = co_await araya::stop_requested();
        }));

    boost::asio::steady_timer cancel_timer{io, 30ms};
    cancel_timer.async_wait(
        [h](boost::system::error_code) { h.cancel(); });

    io.run();
    CHECK(resumed);
    CHECK(h.state() == araya::fiber_state::inactive);
}

TEST_CASE("stop_requested returns immediately outside a fiber") {
    boost::asio::io_context io;
    bool value = false;
    boost::asio::co_spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            value = co_await araya::stop_requested();
        }),
        boost::asio::detached);

    io.run();
    CHECK_FALSE(value);
}

TEST_CASE("nested coroutine in the same fiber sees the fiber stop token") {
    boost::asio::io_context io;
    bool seen = false;
    auto h = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            co_await wait_until_stopped(seen);
        }));

    boost::asio::steady_timer cancel_timer{io, 50ms};
    cancel_timer.async_wait(
        [h](boost::system::error_code) { h.cancel(); });

    io.run();
    CHECK(seen);
}

TEST_CASE("coroutine outside a araya fiber gets a non-stopping token") {
    boost::asio::io_context io;
    bool non_stopping = false;
    boost::asio::co_spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            auto token = co_await araya::this_stop_token();
            non_stopping = !token.stop_requested();
        }),
        boost::asio::detached);

    io.run();
    CHECK(non_stopping);
}

TEST_CASE("cancelling one fiber does not disturb another") {
    boost::asio::io_context io;
    bool first_seen = false;
    bool second_done = false;

    auto first = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            co_await wait_until_stopped(first_seen);
        }));

    auto second = araya::spawn(
        io.get_executor(),
        araya_test::heap_coroutine([&]() -> araya::task<void> {
            second_done = true;
            co_return;
        }));

    boost::asio::steady_timer cancel_timer{io, 50ms};
    cancel_timer.async_wait(
        [first](boost::system::error_code) { first.cancel(); });

    io.run();
    CHECK(first_seen);
    CHECK(second_done);
    CHECK(first.state() == araya::fiber_state::inactive);
    CHECK(second.state() == araya::fiber_state::active);
}

TEST_CASE("fiber ids are unique and increasing") {
    boost::asio::io_context io;
    auto a = araya::spawn(io.get_executor(),
                            araya_test::heap_coroutine(
                                [&]() -> araya::task<void> {
                                    co_return;
                                }));
    auto b = araya::spawn(io.get_executor(),
                            araya_test::heap_coroutine(
                                [&]() -> araya::task<void> {
                                    co_return;
                                }));

    io.run();
    CHECK(a.id() != b.id());
    CHECK(b.id() > a.id());
}
