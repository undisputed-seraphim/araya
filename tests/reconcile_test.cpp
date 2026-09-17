#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

struct database {
    std::string name;
};

inline constexpr araya::service_key<database> db_key{"example.db", 1};

static std::vector<std::string> g_log;

struct provider_plugin : araya::plugin {
    std::string value;
    std::shared_ptr<database> service;

    araya::task<void> apply(araya::plugin_context& ctx) override {
        service = std::make_shared<database>(value);
        ctx.provide(db_key, service);
        g_log.push_back("active:" + value);
        co_return;
    }

    bool reconfigure(araya::plugin_config const& cfg) override {
        value = cfg.at("value");
        service->name = value;
        g_log.push_back("reconfigured:" + value);
        return true;
    }
};

std::unique_ptr<araya::plugin> make_provider(
    araya::plugin_config const& cfg) {
    auto p = std::make_unique<provider_plugin>();
    p->value = cfg.at("value");
    return p;
}

struct strict_provider_plugin : araya::plugin {
    std::string value;

    araya::task<void> apply(araya::plugin_context& ctx) override {
        ctx.provide(db_key, std::make_shared<database>(value));
        g_log.push_back("strict-active:" + value);
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_strict_provider(
    araya::plugin_config const& cfg) {
    auto p = std::make_unique<strict_provider_plugin>();
    p->value = cfg.at("value");
    return p;
}

struct consumer_plugin : araya::plugin {
    std::string tag;

    araya::task<void> apply(araya::plugin_context& ctx) override {
        g_log.push_back("consume:" + tag + ":" +
                        ctx.require<database>(db_key)->name);
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_consumer(
    araya::plugin_config const& cfg) {
    auto p = std::make_unique<consumer_plugin>();
    p->tag = cfg.at("tag");
    return p;
}

struct consumer_v2_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        g_log.push_back("consume-v2:" + ctx.require<database>(db_key)->name);
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_consumer_v2(
    araya::plugin_config const&) {
    return std::make_unique<consumer_v2_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_db_dep[]{
    {araya::service_id{"example.db", 1}, true}};
static const araya::provision_spec g_db_prov[]{
    {araya::service_id{"example.db", 1}}};

static const araya::plugin_descriptor g_provider_desc{
    "provider", g_no_deps, g_db_prov, &make_provider};
static const araya::plugin_descriptor g_strict_provider_desc{
    "strict-provider", g_no_deps, g_db_prov, &make_strict_provider};
static const araya::plugin_descriptor g_consumer_desc{
    "consumer", g_db_dep, g_no_provs, &make_consumer};
static const araya::plugin_descriptor g_consumer_v2_desc{
    "consumer-v2", g_db_dep, g_no_provs, &make_consumer_v2};

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<araya::runtime> rt =
        std::make_shared<araya::runtime>(io.get_executor());

    template <typename Fn>
    void run(Fn&& fn) {
        g_log.clear();        struct driver {
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

    araya::component_spec spec(araya::plugin_descriptor const* d,
                                 araya::plugin_config cfg = {}) {
        return araya::component_spec{
            std::shared_ptr<araya::plugin_descriptor>(
                const_cast<araya::plugin_descriptor*>(d),
                +[](araya::plugin_descriptor*) noexcept {}),
            std::move(cfg), nullptr, ""};
    }

    araya::desired_component node(std::string path,
                                    araya::component_spec s) {
        return araya::desired_component{std::move(path), std::move(s)};
    }
};

}  // namespace

TEST_CASE("reconcile mounts a desired tree") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        desired.push_back(h.node("db", h.spec(&g_provider_desc,
                                              {{"value", "p1"}})));
        desired.push_back(
            h.node("app", h.spec(&g_consumer_desc, {{"tag", "c1"}})));

        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();
    });
    CHECK(g_log == std::vector<std::string>{"active:p1", "consume:c1:p1"});
}

TEST_CASE("reconciling the same tree retains all fibers") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        desired.push_back(h.node("db", h.spec(&g_provider_desc,
                                              {{"value", "p1"}})));
        co_await rt.reconcile(desired);
        co_await rt.wait_idle();
        auto first_id = rt.fiber_count();

        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();
        CHECK(rt.fiber_count() == first_id);
    });
    CHECK(g_log == std::vector<std::string>{"active:p1"});
}

TEST_CASE("reconcile removes fibers absent from the desired tree") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        desired.push_back(h.node("db", h.spec(&g_provider_desc,
                                              {{"value", "p1"}})));
        desired.push_back(
            h.node("app", h.spec(&g_consumer_desc, {{"tag", "c1"}})));
        co_await rt.reconcile(desired);
        co_await rt.wait_idle();

        desired.clear();
        desired.push_back(h.node("db", h.spec(&g_provider_desc,
                                              {{"value", "p1"}})));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();

        auto root = rt.root_context();
        CHECK(root.find<database>(db_key));
    });
    CHECK(g_log == std::vector<std::string>{"active:p1",
                                            "consume:c1:p1"});
}

TEST_CASE("reconcile replaces a changed implementation") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        desired.push_back(h.node("db", h.spec(&g_provider_desc,
                                              {{"value", "p1"}})));
        desired.push_back(
            h.node("app", h.spec(&g_consumer_desc, {{"tag", "c1"}})));
        co_await rt.reconcile(desired);
        co_await rt.wait_idle();

        desired.clear();
        desired.push_back(h.node("db", h.spec(&g_provider_desc,
                                              {{"value", "p1"}})));
        desired.push_back(h.node("app", h.spec(&g_consumer_v2_desc)));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();
    });
    CHECK(g_log == std::vector<std::string>{"active:p1", "consume:c1:p1",
                                            "consume-v2:p1"});
}

TEST_CASE("reconcile applies supported config updates in place") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        desired.push_back(h.node("db", h.spec(&g_provider_desc,
                                              {{"value", "p1"}})));
        co_await rt.reconcile(desired);
        co_await rt.wait_idle();

        desired.clear();
        desired.push_back(h.node("db", h.spec(&g_provider_desc,
                                              {{"value", "p2"}})));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();

        auto root = rt.root_context();
        auto db = root.find<database>(db_key);
        REQUIRE(db);
        CHECK((*db)->name == "p2");
    });
    CHECK(g_log == std::vector<std::string>{"active:p1", "reconfigured:p2"});
}

TEST_CASE("unsupported config updates replace the fiber") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        desired.push_back(h.node("db", h.spec(&g_strict_provider_desc,
                                              {{"value", "p1"}})));
        co_await rt.reconcile(desired);
        co_await rt.wait_idle();

        desired.clear();
        desired.push_back(h.node("db", h.spec(&g_strict_provider_desc,
                                              {{"value", "p2"}})));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();

        auto root = rt.root_context();
        auto db = root.find<database>(db_key);
        REQUIRE(db);
        CHECK((*db)->name == "p2");
    });
    CHECK(g_log == std::vector<std::string>{"strict-active:p1",
                                            "strict-active:p2"});
}

TEST_CASE("reconcile leaves directly mounted fibers alone") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        auto direct = co_await rt.mount(
            h.spec(&g_provider_desc, {{"value", "direct"}}));
        co_await rt.wait_idle();

        std::vector<araya::desired_component> desired;
        desired.push_back(
            h.node("app", h.spec(&g_consumer_desc, {{"tag", "c1"}})));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();

        CHECK(rt.state_of(direct.id()) == araya::fiber_state::active);
    });
    CHECK(std::find(g_log.begin(), g_log.end(), "active:direct") !=
          g_log.end());
    CHECK(std::find(g_log.begin(), g_log.end(), "consume:c1:direct") !=
          g_log.end());
}
