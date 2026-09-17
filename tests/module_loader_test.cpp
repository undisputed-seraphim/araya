#include <catch2/catch_test_macros.hpp>

#include "araya/module_loader.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#ifndef TEST_MODULE_PATH
#error "TEST_MODULE_PATH must point at the test module shared library"
#endif

namespace {

inline constexpr araya::service_id db_id{"example.db", 1};

struct db_service {
    const char* (*name)(void* self);
};

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<araya::runtime> rt =
        std::make_shared<araya::runtime>(io.get_executor());

    template <typename Fn>
    void run(Fn&& fn) {        struct driver {
            std::decay_t<Fn> fn;
            harness* self;
            araya::task<void> operator()() { co_await fn(*self->rt); }
        };
        boost::asio::co_spawn(io.get_executor(),
                              driver{std::forward<Fn>(fn), this},
                              boost::asio::detached);
        io.run();
        io.restart();
    }
};

bool marker_exists() {
    return std::filesystem::exists("/tmp/araya_module_unloaded.marker");
}

void remove_marker() {
    std::filesystem::remove("/tmp/araya_module_unloaded.marker");
}

}  // namespace

TEST_CASE("module loader mounts and runs a dlopen'ed module") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        araya::module_loader loader;
        auto desc = loader.load(TEST_MODULE_PATH);

        auto fh = co_await rt.mount(araya::component_spec{
            desc, {{"tag", "so1"}}, nullptr, ""});
        co_await rt.wait_idle();
        CHECK(rt.state_of(fh.id()) == araya::fiber_state::active);

        auto root = rt.root_context();
        auto b = root.find_binding(db_id);
        REQUIRE(b);
        auto* db = static_cast<db_service*>(b->value.get());
        CHECK(std::string(db->name(db)) == "so1");

        co_await rt.retire(fh);
        co_await rt.wait_idle();
    });
}

TEST_CASE("module unloads after its last descriptor reference dies") {
    remove_marker();
    {
        harness h;
        h.run([&](araya::runtime& rt) -> araya::task<void> {
            araya::module_loader loader;
            auto desc = loader.load(TEST_MODULE_PATH);

            auto fh = co_await rt.mount(araya::component_spec{
                desc, {{"tag", "so2"}}, nullptr, ""});
            co_await rt.wait_idle();
            CHECK(rt.state_of(fh.id()) == araya::fiber_state::active);

            co_await rt.retire(fh);
            co_await rt.wait_idle();
        });
        // the descriptor was captured by the run lambda and released when it
        // completed; the module should now be unloaded
    }
    CHECK(marker_exists());
    remove_marker();
}

TEST_CASE("module stays loaded while fibers still reference it") {
    remove_marker();
    std::shared_ptr<araya::plugin_descriptor> held;
    {
        harness h;
        h.run([&](araya::runtime& rt) -> araya::task<void> {
            araya::module_loader loader;
            held = loader.load(TEST_MODULE_PATH);

            auto fh = co_await rt.mount(araya::component_spec{
                held, {{"tag", "so3"}}, nullptr, ""});
            co_await rt.wait_idle();
            CHECK(rt.state_of(fh.id()) == araya::fiber_state::active);
            co_await rt.retire(fh);
            co_await rt.wait_idle();
        });
        CHECK_FALSE(marker_exists());
    }
    held.reset();
    CHECK(marker_exists());
    remove_marker();
}

TEST_CASE("module loader reports missing files") {
    araya::module_loader loader;
    CHECK_THROWS_AS(loader.load("/nonexistent/definitely/missing.so"),
                    std::runtime_error);
}
