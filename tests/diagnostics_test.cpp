#include <catch2/catch_test_macros.hpp>

#include "medulla/plugin.hpp"
#include "medulla/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

struct database {
    std::string name;
};

inline constexpr medulla::service_key<database> db_key{"example.db", 1};
inline constexpr medulla::service_key<database> x_key{"example.x", 1};
inline constexpr medulla::service_key<database> y_key{"example.y", 1};

struct provider_plugin : medulla::plugin {
    std::string value;

    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        ctx.provide(db_key, std::make_shared<database>(value));
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_provider(
    medulla::plugin_config const& cfg) {
    auto p = std::make_unique<provider_plugin>();
    p->value = cfg.at("value");
    return p;
}

struct self_plugin : medulla::plugin {
    bool optional = false;

    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        ctx.provide(db_key, std::make_shared<database>("self"));
        if (!optional)
            (void)ctx.require<database>(db_key);
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_self_required(
    medulla::plugin_config const&) {
    return std::make_unique<self_plugin>();
}

std::unique_ptr<medulla::plugin> make_self_optional(
    medulla::plugin_config const&) {
    auto p = std::make_unique<self_plugin>();
    p->optional = true;
    return p;
}

struct consumer_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        (void)ctx.require<database>(db_key);
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_consumer(
    medulla::plugin_config const&) {
    return std::make_unique<consumer_plugin>();
}

// A provides x, declares y; B provides y, declares x: a mutual cycle.
struct x_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        ctx.provide(x_key, std::make_shared<database>("x"));
        (void)ctx.require<database>(y_key);
        co_return;
    }
};

struct y_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        ctx.provide(y_key, std::make_shared<database>("y"));
        (void)ctx.require<database>(x_key);
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_x(medulla::plugin_config const&) {
    return std::make_unique<x_plugin>();
}

std::unique_ptr<medulla::plugin> make_y(medulla::plugin_config const&) {
    return std::make_unique<y_plugin>();
}

static constexpr std::span<medulla::dependency_spec const> g_no_deps{};
static constexpr std::span<medulla::provision_spec const> g_no_provs{};
static const medulla::dependency_spec g_db_dep[]{
    {medulla::service_id{"example.db", 1}, true}};
static const medulla::dependency_spec g_db_optional[]{
    {medulla::service_id{"example.db", 1}, false}};
static const medulla::dependency_spec g_x_dep[]{
    {medulla::service_id{"example.x", 1}, true}};
static const medulla::dependency_spec g_y_dep[]{
    {medulla::service_id{"example.y", 1}, true}};
static const medulla::provision_spec g_db_prov[]{
    {medulla::service_id{"example.db", 1}}};
static const medulla::provision_spec g_x_prov[]{
    {medulla::service_id{"example.x", 1}}};
static const medulla::provision_spec g_y_prov[]{
    {medulla::service_id{"example.y", 1}}};

static const medulla::plugin_descriptor g_provider_desc{
    "provider", g_no_deps, g_db_prov, &make_provider};
static const medulla::plugin_descriptor g_consumer_desc{
    "consumer", g_db_dep, g_no_provs, &make_consumer};
static const medulla::plugin_descriptor g_self_required_desc{
    "self-required", g_db_dep, g_db_prov, &make_self_required};
static const medulla::plugin_descriptor g_self_optional_desc{
    "self-optional", g_db_optional, g_db_prov, &make_self_optional};
static const medulla::plugin_descriptor g_x_desc{
    "x", g_y_dep, g_x_prov, &make_x};
static const medulla::plugin_descriptor g_y_desc{
    "y", g_x_dep, g_y_prov, &make_y};

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<medulla::runtime> rt =
        std::make_shared<medulla::runtime>(io.get_executor());
    std::vector<medulla::diagnostic> diagnostics;

    template <typename Fn>
    void run(Fn&& fn) {
        diagnostics.clear();
        rt->on_diagnostic([this](medulla::diagnostic const& d) {
            diagnostics.push_back(d);
        });
        boost::asio::co_spawn(
            io.get_executor(),
            [this, fn = std::forward<Fn>(fn)]() mutable -> medulla::task<void> {
                co_await fn(*rt);
            }(),
            boost::asio::detached);
        io.run();
        io.restart();
    }

    medulla::component_spec spec(medulla::plugin_descriptor const* d,
                                 medulla::plugin_config cfg = {}) {
        return medulla::component_spec{std::shared_ptr<medulla::plugin_descriptor>(
                                           const_cast<medulla::plugin_descriptor*>(d),
                                           [](auto*) {}),
                                       std::move(cfg), nullptr, ""};
    }
};

bool has_cycle(std::vector<medulla::diagnostic> const& ds,
               std::vector<medulla::fiber_id> const& members) {
    for (auto const& d : ds) {
        if (d.kind != medulla::diagnostic_kind::cycle)
            continue;
        if (d.fibers.size() != members.size())
            continue;
        std::vector<medulla::fiber_id> sorted = d.fibers;
        std::sort(sorted.begin(), sorted.end());
        auto want = members;
        std::sort(want.begin(), want.end());
        if (sorted == want)
            return true;
    }
    return false;
}

bool has_conflict(std::vector<medulla::diagnostic> const& ds,
                  std::string const& key_name,
                  std::uint32_t version,
                  std::vector<medulla::fiber_id> const& members) {
    for (auto const& d : ds) {
        if (d.kind != medulla::diagnostic_kind::conflict)
            continue;
        if (d.key_name != key_name || d.key_version != version)
            continue;
        if (d.fibers.size() != members.size())
            continue;
        std::vector<medulla::fiber_id> sorted = d.fibers;
        std::sort(sorted.begin(), sorted.end());
        auto want = members;
        std::sort(want.begin(), want.end());
        if (sorted == want)
            return true;
    }
    return false;
}

}  // namespace

TEST_CASE("mutual dependency cycle is diagnosed and both fibers stay inactive") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto a = co_await rt.mount(h.spec(&g_x_desc));
        co_await rt.wait_idle();
        auto b = co_await rt.mount(h.spec(&g_y_desc));
        co_await rt.wait_idle();

        CHECK(rt.state_of(a.id()) == medulla::fiber_state::inactive);
        CHECK(rt.state_of(b.id()) == medulla::fiber_state::inactive);
        CHECK(rt.error_of(a.id()) != nullptr);
        CHECK(rt.error_of(b.id()) != nullptr);
        REQUIRE(has_cycle(h.diagnostics, {a.id(), b.id()}));
    });
}

TEST_CASE("required self-provision is diagnosed and stays inactive") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto s = co_await rt.mount(h.spec(&g_self_required_desc));
        co_await rt.wait_idle();

        CHECK(rt.state_of(s.id()) == medulla::fiber_state::inactive);
        CHECK(rt.error_of(s.id()) != nullptr);
        REQUIRE(has_cycle(h.diagnostics, {s.id()}));
    });
}

TEST_CASE("optional self-provision is diagnosed, activates, and does not "
          "oscillate") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto s = co_await rt.mount(h.spec(&g_self_optional_desc));
        co_await rt.wait_idle();

        CHECK(rt.state_of(s.id()) == medulla::fiber_state::active);
        auto root = rt.root_context();
        auto db = root.find<database>(db_key);
        REQUIRE(db);
        REQUIRE(has_cycle(h.diagnostics, {s.id()}));
    });
}

TEST_CASE("two providers of one key in one scope are diagnosed as a conflict") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto p1 = co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        auto p2 = co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p2"}}));
        co_await rt.wait_idle();

        CHECK(rt.state_of(p1.id()) == medulla::fiber_state::active);
        CHECK(rt.state_of(p2.id()) == medulla::fiber_state::active);
        REQUIRE(has_conflict(h.diagnostics, "example.db", 1, {p1.id(), p2.id()}));
    });
}

TEST_CASE("providers in different scopes are not a conflict") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto child = rt.root()->make_child();
        auto root_spec = h.spec(&g_provider_desc, {{"value", "root"}});
        root_spec.parent = rt.root();
        auto child_spec = h.spec(&g_provider_desc, {{"value", "child"}});
        child_spec.parent = child;

        co_await rt.mount(std::move(root_spec));
        co_await rt.mount(std::move(child_spec));
        co_await rt.wait_idle();

        CHECK(h.diagnostics.empty());
    });
}

TEST_CASE("healthy graphs produce no diagnostics") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        co_await rt.mount(h.spec(&g_consumer_desc));
        co_await rt.wait_idle();
        CHECK(h.diagnostics.empty());
    });
}
