#include <catch2/catch_test_macros.hpp>

#include "medulla/plugin.hpp"
#include "medulla/runtime.hpp"
#include "medulla/timer/timer.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <memory>
#include <span>

namespace {

using namespace std::chrono_literals;
using namespace medulla::timer;

static int g_count = 0;

// A fiber that owns one long timeout: unloading it must cancel the timer
// before it fires.
struct timer_owner_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        auto service = ctx.require<timer_service>(timer_key);
        (void)service->timeout(ctx, 300ms, [] { ++g_count; });
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_timer_owner(
    medulla::plugin_config const&) {
    return std::make_unique<timer_owner_plugin>();
}

static constexpr std::span<medulla::dependency_spec const> g_no_deps{};
static constexpr std::span<medulla::provision_spec const> g_no_provs{};
static const medulla::dependency_spec g_timer_dep[]{
    {medulla::service_id{"timer", 1}, true}};
static const medulla::plugin_descriptor g_timer_owner_desc{
    "timer-owner", g_timer_dep, g_no_provs, &make_timer_owner};

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<medulla::runtime> rt =
        std::make_shared<medulla::runtime>(io.get_executor());

    template <typename Fn>
    void run(Fn&& fn) {
        g_count = 0;
        struct driver {
            std::decay_t<Fn> fn;
            harness* self;
            medulla::task<void> operator()() { co_await fn(*self->rt); }
        };
        boost::asio::co_spawn(io.get_executor(),
                              driver{std::forward<Fn>(fn), this},
                              boost::asio::detached);
        io.run();
        io.restart();
    }

    medulla::component_spec spec(medulla::plugin_descriptor const* d,
                                 medulla::plugin_config cfg = {}) {
        return medulla::component_spec{
            std::shared_ptr<medulla::plugin_descriptor>(
                const_cast<medulla::plugin_descriptor*>(d),
                [](auto*) {}),
            std::move(cfg), nullptr, ""};
    }
};

}  // namespace

TEST_CASE("sleep awaits at least the requested delay") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.spec(&medulla::timer::plugin_descriptor()));
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto service =
            root_ctx.require<timer_service>(timer_key).shared();

        auto start = std::chrono::steady_clock::now();
        co_await service->sleep(30ms);
        auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(elapsed >= 25ms);
    });
}

TEST_CASE("a timeout fires exactly once") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.spec(&medulla::timer::plugin_descriptor()));
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto service =
            root_ctx.require<timer_service>(timer_key).shared();
        (void)service->timeout(root_ctx, 10ms, [] { ++g_count; });
        co_await service->sleep(30ms);
        CHECK(g_count == 1);
        co_await service->sleep(20ms);
        CHECK(g_count == 1);
    });
}

TEST_CASE("an interval repeats until it returns false") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.spec(&medulla::timer::plugin_descriptor()));
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto service =
            root_ctx.require<timer_service>(timer_key).shared();
        int n = 0;
        (void)service->interval(root_ctx, 5ms, [&n] { return ++n < 3; });
        co_await service->sleep(40ms);
        CHECK(n == 3);
    });
}

TEST_CASE("unloading the owning fiber cancels its timers") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.spec(&medulla::timer::plugin_descriptor()));
        auto owner = co_await rt.mount(h.spec(&g_timer_owner_desc));
        co_await rt.wait_idle();

        auto root_ctx = rt.root_context();
        auto service =
            root_ctx.require<timer_service>(timer_key).shared();

        co_await rt.retire(owner);
        co_await rt.wait_idle();
        co_await service->sleep(350ms);
        CHECK(g_count == 0);
    });
}
