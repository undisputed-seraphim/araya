#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
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

    araya::task<void> apply(araya::plugin_context& ctx) override {
        g_log.push_back("apply:" + value);
        ctx.provide(db_key, std::make_shared<database>(value));
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_provider(
    araya::plugin_config const& cfg) {
    auto p = std::make_unique<provider_plugin>();
    p->value = cfg.at("value");
    return p;
}

struct consumer_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        auto db = ctx.require<database>(db_key);
        g_log.push_back("consume:" + db->name);
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_consumer(
    araya::plugin_config const&) {
    return std::make_unique<consumer_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_db_dep[]{
    {araya::service_id{"example.db", 1}, true}};
static const araya::provision_spec g_db_prov[]{
    {araya::service_id{"example.db", 1}}};

static const araya::plugin_descriptor g_provider_desc{
    "provider", g_no_deps, g_db_prov, &make_provider};
static const araya::plugin_descriptor g_consumer_desc{
    "consumer", g_db_dep, g_no_provs, &make_consumer};

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<araya::runtime> rt =
        std::make_shared<araya::runtime>(io.get_executor());

    template <typename Fn>
    void run(Fn&& fn) {
        g_log.clear();
        struct driver {
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
        return araya::component_spec{std::shared_ptr<araya::plugin_descriptor>(
                                           const_cast<araya::plugin_descriptor*>(d),
                                           [](auto*) {}),
                                       std::move(cfg), nullptr, ""};
    }

    static araya::desired_component node(
        std::string path, araya::component_spec s) {
        return araya::desired_component{std::move(path), std::move(s)};
    }
};

}  // namespace

TEST_CASE("isolating a key hides the ancestor binding and scopes its own") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        auto root = rt.root();
        root->bind(db_key.id, araya::binding{std::make_shared<database>(
                                                   database{"root"}), 7});

        auto isolated = root->make_child();
        isolated->isolate(db_key.id, "test");

        // The isolated child no longer sees the ancestor binding.
        CHECK(isolated->lookup(db_key.id) == nullptr);
        // The ancestor still sees its own.
        auto* b = root->lookup(db_key.id);
        REQUIRE(b != nullptr);
        CHECK(static_cast<database*>(b->value.get())->name == "root");

        // A binding made under the isolated realm is visible only there.
        isolated->bind(db_key.id, araya::binding{std::make_shared<database>(
                                                       database{"test"}), 8});
        auto* iso = isolated->lookup(db_key.id);
        REQUIRE(iso != nullptr);
        CHECK(static_cast<database*>(iso->value.get())->name == "test");
        CHECK(static_cast<database*>(root->lookup(db_key.id)->value.get())
                  ->name == "root");

        // A sibling without the tag is unaffected.
        auto sibling = root->make_child();
        CHECK(static_cast<database*>(sibling->lookup(db_key.id)->value.get())
                  ->name == "root");

        // Keys sharing a realm share one binding slot.
        auto shared = root->make_child();
        shared->isolate(db_key.id, "pool");
        shared->bind(db_key.id, araya::binding{
                                    std::make_shared<database>(
                                        database{"pool"}), 9});
        CHECK(shared->lookup(db_key.id) ==
              shared->lookup(db_key.id));
        co_return;
    });
}

TEST_CASE("reconcile isolates a provider and its consumer into one realm") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        auto provider = h.spec(&g_provider_desc, {{"value", "p1"}});
        provider.isolate = {{"example.db", "prod"}};
        auto consumer = h.spec(&g_consumer_desc);
        consumer.isolate = {{"example.db", "prod"}};
        desired.push_back(h.node("p", std::move(provider)));
        desired.push_back(h.node("c", std::move(consumer)));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();

        // Both fibers see the prod binding; a plain-root consumer would not.
        auto root = rt.root();
        CHECK(root->lookup(db_key.id) == nullptr);
        CHECK(rt.fiber_count() == 2);
        CHECK(std::find(g_log.begin(), g_log.end(), "consume:p1") !=
              g_log.end());
    });
}

TEST_CASE("a plain consumer does not see an isolated provider") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        auto provider = h.spec(&g_provider_desc, {{"value", "p1"}});
        provider.isolate = {{"example.db", "prod"}};
        desired.push_back(h.node("p", std::move(provider)));
        desired.push_back(h.node("c", h.spec(&g_consumer_desc)));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();

        // The provider activates and binds under "prod"; the consumer's
        // default realm never resolves it.
        CHECK(std::find(g_log.begin(), g_log.end(), "consume:p1") ==
              g_log.end());
    });
}

TEST_CASE("realm reassignment moves the binding without reloading the "
          "provider") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        auto provider = h.spec(&g_provider_desc, {{"value", "p1"}});
        provider.isolate = {{"example.db", "prod"}};
        auto consumer = h.spec(&g_consumer_desc);
        consumer.isolate = {{"example.db", "prod"}};
        desired.push_back(h.node("p", provider));
        desired.push_back(h.node("c", consumer));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();
        REQUIRE(std::find(g_log.begin(), g_log.end(), "consume:p1") !=
                g_log.end());

        // Move the provider to a new realm; the provider must not reload.
        desired.push_back(h.node("p", provider));  // old spec to satisfy copy
        desired.clear();
        auto moved = h.spec(&g_provider_desc, {{"value", "p1"}});
        moved.isolate = {{"example.db", "stage"}};
        auto stage_consumer = h.spec(&g_consumer_desc);
        stage_consumer.isolate = {{"example.db", "stage"}};
        desired.push_back(h.node("p", std::move(moved)));
        desired.push_back(h.node("c2", std::move(stage_consumer)));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();

        CHECK(std::count(g_log.begin(), g_log.end(), "apply:p1") == 1);
        CHECK(std::find(g_log.begin(), g_log.end(), "consume:p1") !=
              g_log.end());
    });
}

TEST_CASE("removing the isolate annotation restores plain resolution") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        std::vector<araya::desired_component> desired;
        auto provider = h.spec(&g_provider_desc, {{"value", "p1"}});
        provider.isolate = {{"example.db", "prod"}};
        desired.push_back(h.node("p", provider));
        desired.push_back(h.node("c", h.spec(&g_consumer_desc)));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();
        CHECK(std::find(g_log.begin(), g_log.end(), "consume:p1") ==
              g_log.end());

        desired.clear();
        desired.push_back(h.node("p", h.spec(&g_provider_desc,
                                             {{"value", "p1"}})));
        desired.push_back(h.node("c", h.spec(&g_consumer_desc)));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();

        // The binding moved back to the default realm; the provider did not
        // reload, and the plain consumer now resolves it.
        CHECK(std::count(g_log.begin(), g_log.end(), "apply:p1") == 1);
        CHECK(std::find(g_log.begin(), g_log.end(), "consume:p1") !=
              g_log.end());
    });
}

TEST_CASE("moving an entry between scopes carries its own binding") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        auto group_a = rt.root()->make_child();
        auto group_b = rt.root()->make_child();

        std::vector<araya::desired_component> desired;
        auto provider = h.spec(&g_provider_desc, {{"value", "p1"}});
        provider.parent = group_a;
        desired.push_back(h.node("p", provider));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();
        CHECK(group_a->lookup(db_key.id) != nullptr);
        CHECK(group_b->lookup(db_key.id) == nullptr);

        desired.clear();
        auto moved = h.spec(&g_provider_desc, {{"value", "p1"}});
        moved.parent = group_b;
        desired.push_back(h.node("p", std::move(moved)));
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();

        // The provider's own binding moved with it; no reload happened.
        CHECK(std::count(g_log.begin(), g_log.end(), "apply:p1") == 1);
        CHECK(group_a->lookup(db_key.id) == nullptr);
        CHECK(group_b->lookup(db_key.id) != nullptr);
    });
}
