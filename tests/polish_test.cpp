#include <catch2/catch_test_macros.hpp>

#include "medulla/plugin.hpp"
#include "medulla/runtime.hpp"

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

inline constexpr medulla::service_key<database> db_key{"example.db", 1};

static std::vector<std::string> g_log;

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

struct broken_consumer : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        g_log.push_back("attempt");
        (void)ctx.require<database>(db_key);
        throw std::runtime_error("consumer always fails");
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_broken_consumer(
    medulla::plugin_config const&) {
    return std::make_unique<broken_consumer>();
}

struct metadata_consumer : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        auto db = ctx.require<database>(db_key);
        for (auto const& [k, v] : db.metadata())
            g_log.push_back("meta:" + k + "=" + v);
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_metadata_consumer(
    medulla::plugin_config const&) {
    return std::make_unique<metadata_consumer>();
}

static constexpr std::span<medulla::dependency_spec const> g_no_deps{};
static constexpr std::span<medulla::provision_spec const> g_no_provs{};
static const medulla::dependency_spec g_db_dep[]{
    {medulla::service_id{"example.db", 1}, true}};
static const medulla::dependency_spec g_db_meta_dep[]{
    {medulla::service_id{"example.db", 1}, true,
     {{"mode", "readonly"}}}};
static const medulla::provision_spec g_db_prov[]{
    {medulla::service_id{"example.db", 1}}};

static const medulla::plugin_descriptor g_provider_desc{
    "provider", g_no_deps, g_db_prov, &make_provider};
static const medulla::plugin_descriptor g_broken_consumer_desc{
    "broken-consumer", g_db_dep, g_no_provs, &make_broken_consumer};
static const medulla::plugin_descriptor g_metadata_consumer_desc{
    "metadata-consumer", g_db_meta_dep, g_no_provs, &make_metadata_consumer};

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<medulla::runtime> rt =
        std::make_shared<medulla::runtime>(io.get_executor());

    template <typename Fn>
    void run(Fn&& fn) {
        g_log.clear();
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
        return medulla::component_spec{std::shared_ptr<medulla::plugin_descriptor>(
                                           const_cast<medulla::plugin_descriptor*>(d),
                                           [](auto*) {}),
                                       std::move(cfg), nullptr, ""};
    }
};

}  // namespace

TEST_CASE("a raised fiber is not retried on dependency changes, only by "
          "revision") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        auto consumer =
            co_await rt.mount(h.spec(&g_broken_consumer_desc));
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::inactive);
        CHECK(rt.error_of(consumer.id()) != nullptr);
        REQUIRE(std::count(g_log.begin(), g_log.end(), "attempt") == 1);

        // Environment change: the provider is replaced. The paper's failure
        // extension withholds re-entry, so the raised fiber stays down.
        auto p1 = co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p2"}}));
        (void)p1;
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::inactive);
        CHECK(std::count(g_log.begin(), g_log.end(), "attempt") == 1);

        // A revision (reinsertion) clears the outcome and retries.
        co_await rt.retire(consumer);
        co_await rt.mount(h.spec(&g_broken_consumer_desc));
        co_await rt.wait_idle();
        CHECK(std::count(g_log.begin(), g_log.end(), "attempt") == 2);
    });
}

TEST_CASE("interception metadata merges at access with context priority") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        // The consumer declares mode=readonly; the context carries nothing
        // yet, so the lease carries the declaration.
        co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        auto consumer =
            co_await rt.mount(h.spec(&g_metadata_consumer_desc));
        co_await rt.wait_idle();
        CHECK(std::find(g_log.begin(), g_log.end(), "meta:mode=readonly") !=
              g_log.end());

        // Context metadata now overrides and extends (Definition 27,
        // right-biased: the context takes priority).
        rt.root()->set_metadata(db_key.id, {{"mode", "readwrite"},
                                            {"quota", "10"}});
        co_await rt.retire(consumer);
        co_await rt.mount(h.spec(&g_metadata_consumer_desc));
        co_await rt.wait_idle();
        CHECK(std::find(g_log.begin(), g_log.end(), "meta:mode=readwrite") !=
              g_log.end());
        CHECK(std::find(g_log.begin(), g_log.end(), "meta:quota=10") !=
              g_log.end());
    });
}
