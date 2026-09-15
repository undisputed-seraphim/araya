#include <catch2/catch_test_macros.hpp>

#include "coro_util.hpp"

#include "medulla/task.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/system_error.hpp>

#include <chrono>
#include <stdexcept>

using namespace std::chrono_literals;

namespace {

medulla::task<void> wait_until_stopped(bool& seen) {
    auto token = co_await medulla::this_stop_token();
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
    auto h = medulla::spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<int> {
            result = 42;
            co_return 42;
        }));

    CHECK(h.state() == medulla::fiber_state::loading);
    io.run();

    CHECK(result == 42);
    CHECK(h.state() == medulla::fiber_state::active);
}

TEST_CASE("exception marks fiber inactive") {
    boost::asio::io_context io;
    auto h = medulla::spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<void> {
            throw std::runtime_error("boom");
            co_return;
        }));

    io.run();
    CHECK(h.state() == medulla::fiber_state::inactive);
}

TEST_CASE("cancel is observed cooperatively through this_stop_token") {
    boost::asio::io_context io;
    bool seen = false;
    auto h = medulla::spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<void> {
            co_await wait_until_stopped(seen);
        }));

    boost::asio::steady_timer cancel_timer{io, 50ms};
    cancel_timer.async_wait(
        [h](boost::system::error_code) { h.cancel(); });

    io.run();
    CHECK(seen);
    CHECK(h.state() == medulla::fiber_state::inactive);
}

TEST_CASE("cancel does not abort an in-flight operation") {
    boost::asio::io_context io;
    bool timer_done = false;
    bool stop_seen = false;
    auto h = medulla::spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<void> {
            auto timer = boost::asio::steady_timer(
                co_await boost::asio::this_coro::executor, 60ms);
            co_await timer.async_wait(boost::asio::use_awaitable);
            timer_done = true;
            auto token = co_await medulla::this_stop_token();
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
    CHECK(h.state() == medulla::fiber_state::inactive);
}

TEST_CASE("stop_requested resumes a suspended fiber") {
    boost::asio::io_context io;
    bool resumed = false;
    auto h = medulla::spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<void> {
            resumed = co_await medulla::stop_requested();
        }));

    boost::asio::steady_timer cancel_timer{io, 30ms};
    cancel_timer.async_wait(
        [h](boost::system::error_code) { h.cancel(); });

    io.run();
    CHECK(resumed);
    CHECK(h.state() == medulla::fiber_state::inactive);
}

TEST_CASE("stop_requested returns immediately outside a fiber") {
    boost::asio::io_context io;
    bool value = false;
    boost::asio::co_spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<void> {
            value = co_await medulla::stop_requested();
        }),
        boost::asio::detached);

    io.run();
    CHECK_FALSE(value);
}

TEST_CASE("nested coroutine in the same fiber sees the fiber stop token") {
    boost::asio::io_context io;
    bool seen = false;
    auto h = medulla::spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<void> {
            co_await wait_until_stopped(seen);
        }));

    boost::asio::steady_timer cancel_timer{io, 50ms};
    cancel_timer.async_wait(
        [h](boost::system::error_code) { h.cancel(); });

    io.run();
    CHECK(seen);
}

TEST_CASE("coroutine outside a medulla fiber gets a non-stopping token") {
    boost::asio::io_context io;
    bool non_stopping = false;
    boost::asio::co_spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<void> {
            auto token = co_await medulla::this_stop_token();
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

    auto first = medulla::spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<void> {
            co_await wait_until_stopped(first_seen);
        }));

    auto second = medulla::spawn(
        io.get_executor(),
        medulla_test::heap_coroutine([&]() -> medulla::task<void> {
            second_done = true;
            co_return;
        }));

    boost::asio::steady_timer cancel_timer{io, 50ms};
    cancel_timer.async_wait(
        [first](boost::system::error_code) { first.cancel(); });

    io.run();
    CHECK(first_seen);
    CHECK(second_done);
    CHECK(first.state() == medulla::fiber_state::inactive);
    CHECK(second.state() == medulla::fiber_state::active);
}

TEST_CASE("fiber ids are unique and increasing") {
    boost::asio::io_context io;
    auto a = medulla::spawn(io.get_executor(),
                            medulla_test::heap_coroutine(
                                [&]() -> medulla::task<void> {
                                    co_return;
                                }));
    auto b = medulla::spawn(io.get_executor(),
                            medulla_test::heap_coroutine(
                                [&]() -> medulla::task<void> {
                                    co_return;
                                }));

    io.run();
    CHECK(a.id() != b.id());
    CHECK(b.id() > a.id());
}
