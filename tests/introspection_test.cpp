#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

struct database {
    std::string name;
};

inline constexpr araya::service_key<database> db_key{"example.db", 1};

struct provider_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        ctx.provide(db_key, std::make_shared<database>("p"));
        co_return;
    }
};

struct consumer_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        (void)ctx.require<database>(db_key);
        co_return;
    }
};

struct broken_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        (void)ctx.require<database>(db_key);
        throw std::runtime_error("boom");
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_provider(
    araya::plugin_config const&) {
    return std::make_unique<provider_plugin>();
}

std::unique_ptr<araya::plugin> make_consumer(araya::plugin_config const&) {
    return std::make_unique<consumer_plugin>();
}

std::unique_ptr<araya::plugin> make_broken(araya::plugin_config const&) {
    return std::make_unique<broken_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_db_dep[]{
    {araya::service_id{"example.db", 1}, true, {}}};
static const araya::provision_spec g_db_prov[]{
    {araya::service_id{"example.db", 1}}};

static const araya::plugin_descriptor g_desc_P{"P", g_no_deps, g_db_prov,
                                               &make_provider};
static const araya::plugin_descriptor g_desc_C{"C", g_db_dep, g_no_provs,
                                               &make_consumer};
static const araya::plugin_descriptor g_desc_B{"B", g_db_dep, g_no_provs,
                                               &make_broken};

std::shared_ptr<araya::plugin_descriptor> shared_desc(
    araya::plugin_descriptor const& d) {
    return std::shared_ptr<araya::plugin_descriptor>(
        const_cast<araya::plugin_descriptor*>(&d),
        [](araya::plugin_descriptor*) {});
}

// Captures the plugin_context::root() result observed during apply().
inline std::shared_ptr<araya::context> g_seen_root;

struct spy_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        g_seen_root = ctx.root();
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_spy(araya::plugin_config const&) {
    return std::make_unique<spy_plugin>();
}

static const araya::plugin_descriptor g_desc_spy{"spy", g_no_deps,
                                                 g_no_provs, &make_spy};

// Runs one coroutine body on the io_context and drains it.
void run(boost::asio::io_context& io,
         std::move_only_function<araya::task<void>()> body) {
    auto b = std::make_shared<std::move_only_function<araya::task<void>()>>(
        std::move(body));
    boost::asio::co_spawn(
        io.get_executor(),
        [b]() -> araya::task<void> { co_await (*b)(); },
        boost::asio::detached);
    io.run();
    io.restart();
}

}  // namespace

TEST_CASE("plugin_context::root reaches the runtime root from any scope") {
    boost::asio::io_context io;
    auto rt = std::make_shared<araya::runtime>(io.get_executor());

    // Host side: the root context is its own root and has no parent.
    auto host = rt->root_context();
    REQUIRE(host.root() == rt->root());
    REQUIRE(host.root()->parent() == nullptr);

    // Fiber side: a component mounted under a child context still sees
    // the runtime root, not its immediate scope.
    g_seen_root = nullptr;
    auto child = rt->root()->make_child();
    run(io, [rt, child]() -> araya::task<void> {
        auto spec = araya::component_spec{
            shared_desc(g_desc_spy), {}, child, "spy", {}};
        co_await rt->mount(std::move(spec));
        co_await rt->wait_idle();
    });
    REQUIRE(g_seen_root != nullptr);
    REQUIRE(g_seen_root == rt->root());
    REQUIRE(g_seen_root->parent() == nullptr);
}

TEST_CASE("fibers() snapshots ids, states, declarations, and views") {
    boost::asio::io_context io;
    auto rt = std::make_shared<araya::runtime>(io.get_executor());

    araya::fiber_handle p, c, b;
    run(io, [&]() -> araya::task<void> {
        p = co_await rt->mount(araya::component_spec{
            shared_desc(g_desc_P), {}, nullptr, "p-instance", {}});
        c = co_await rt->mount(araya::component_spec{
            shared_desc(g_desc_C), {}, nullptr, "", {}});
        b = co_await rt->mount(araya::component_spec{
            shared_desc(g_desc_B), {}, nullptr, "b-instance", {}});
        co_await rt->wait_idle();
    });

    auto snap = rt->fibers();
    REQUIRE(snap.size() == 3);
    REQUIRE(p.id() != c.id());
    REQUIRE(c.id() != b.id());
    REQUIRE(p.id() != b.id());

    auto pinfo = [&](araya::fiber_id id) -> araya::fiber_info const* {
        for (auto const& f : snap)
            if (f.id == id)
                return &f;
        return nullptr;
    };

    auto const* pi = pinfo(p.id());
    auto const* ci = pinfo(c.id());
    auto const* bi = pinfo(b.id());
    REQUIRE(pi != nullptr);
    REQUIRE(ci != nullptr);
    REQUIRE(bi != nullptr);

    // The provider: active, declares the provision, instance name kept.
    CHECK(pi->descriptor == "P");
    CHECK(pi->name == "p-instance");
    CHECK(pi->state == araya::fiber_state::active);
    CHECK(pi->error == nullptr);
    CHECK(pi->parent == 0);
    CHECK(pi->scope == rt->root());
    CHECK(pi->inject.empty());
    REQUIRE(pi->provide.size() == 1);
    CHECK(pi->provide[0] == araya::owned_service_id{db_key.id});

    // The consumer: its committed view names the provider.
    CHECK(ci->descriptor == "C");
    CHECK(ci->state == araya::fiber_state::active);
    REQUIRE(ci->inject.size() == 1);
    CHECK(ci->inject[0] == araya::owned_service_id{db_key.id});
    CHECK(ci->provide.empty());
    REQUIRE(ci->committed.size() == 1);
    CHECK(ci->committed.begin()->first ==
          araya::owned_service_id{db_key.id});
    CHECK(ci->committed.begin()->second == p.id());

    // The broken component: failed with an error, still enumerated.
    CHECK(bi->descriptor == "B");
    CHECK(bi->name == "b-instance");
    CHECK(bi->state == araya::fiber_state::inactive);
    CHECK(bi->error != nullptr);

    // Retiring a fiber removes it from the snapshot.
    run(io, [&]() -> araya::task<void> {
        co_await rt->retire(b);
        co_await rt->wait_idle();
    });
    snap = rt->fibers();
    REQUIRE(snap.size() == 2);
    CHECK(pinfo(b.id()) == nullptr);
}

TEST_CASE("fibers_async returns the snapshot from off-strand callers") {
    boost::asio::io_context io;
    auto rt = std::make_shared<araya::runtime>(io.get_executor());

    run(io, [&]() -> araya::task<void> {
        co_await rt->mount(araya::component_spec{
            shared_desc(g_desc_P), {}, nullptr, "p", {}});
        co_await rt->wait_idle();
    });

    std::vector<araya::fiber_info> seen;
    run(io, [&]() -> araya::task<void> {
        seen = co_await rt->fibers_async();
    });

    REQUIRE(seen.size() == 1);
    CHECK(seen.size() == rt->fibers().size());
    if (!seen.empty())
        CHECK(seen[0].descriptor == "P");
}
