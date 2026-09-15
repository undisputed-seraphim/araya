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

static std::vector<std::string> g_log;

medulla::cleanup_action log_effect(std::string tag) {
    g_log.push_back("setup:" + tag);
    return [tag] { g_log.push_back("cleanup:" + tag); };
}

struct provider_plugin : medulla::plugin {
    std::string value;

    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        ctx.effect([tag = "provide:" + value] {
            return log_effect(tag);
        });
        ctx.provide(db_key, std::make_shared<database>(value));
        g_log.push_back("active:" + value);
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_provider(
    medulla::plugin_config const& cfg) {
    auto p = std::make_unique<provider_plugin>();
    p->value = cfg.at("value");
    return p;
}

struct consumer_plugin : medulla::plugin {
    std::string tag;

    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        auto db = ctx.require<database>(db_key);
        g_log.push_back("consume:" + tag + ":" + db->name);
        ctx.effect([tag = tag, shared = db.shared()] {
            g_log.push_back("c-setup:" + tag);
            return [tag, shared] {
                g_log.push_back("c-cleanup:" + tag + ":" + shared->name);
            };
        });
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_consumer(
    medulla::plugin_config const& cfg) {
    auto p = std::make_unique<consumer_plugin>();
    p->tag = cfg.at("tag");
    return p;
}

struct optional_consumer_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        auto db = ctx.find<database>(db_key);
        g_log.push_back(db ? "opt:" + (*db)->name : "opt:none");
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_optional_consumer(
    medulla::plugin_config const&) {
    return std::make_unique<optional_consumer_plugin>();
}

struct slow_provider_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        auto stopped = co_await medulla::stop_requested();
        if (!stopped) {
            g_log.push_back("slow:active");
            ctx.provide(db_key, std::make_shared<database>("slow"));
        }
    }
};

std::unique_ptr<medulla::plugin> make_slow_provider(
    medulla::plugin_config const&) {
    return std::make_unique<slow_provider_plugin>();
}

struct broken_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        ctx.effect([tag = std::string("broken")] {
            return log_effect(tag);
        });
        throw std::runtime_error("broken apply");
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_broken(medulla::plugin_config const&) {
    return std::make_unique<broken_plugin>();
}

struct undeclared_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        (void)ctx.require<database>(db_key);
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_undeclared(
    medulla::plugin_config const&) {
    return std::make_unique<undeclared_plugin>();
}

static constexpr std::span<medulla::dependency_spec const> g_no_deps{};
static constexpr std::span<medulla::provision_spec const> g_no_provs{};
static const medulla::dependency_spec g_db_dep[]{
    {medulla::service_id{"example.db", 1}, true}};
static const medulla::dependency_spec g_db_optional[]{
    {medulla::service_id{"example.db", 1}, false}};
static const medulla::provision_spec g_db_prov[]{
    {medulla::service_id{"example.db", 1}}};

static const medulla::plugin_descriptor g_provider_desc{
    "provider", g_no_deps, g_db_prov, &make_provider};
static const medulla::plugin_descriptor g_consumer_desc{
    "consumer", g_db_dep, g_no_provs, &make_consumer};
static const medulla::plugin_descriptor g_optional_consumer_desc{
    "optional-consumer", g_db_optional, g_no_provs, &make_optional_consumer};
static const medulla::plugin_descriptor g_slow_provider_desc{
    "slow-provider", g_no_deps, g_db_prov, &make_slow_provider};
static const medulla::plugin_descriptor g_broken_desc{
    "broken", g_no_deps, g_no_provs, &make_broken};
static const medulla::plugin_descriptor g_undeclared_desc{
    "undeclared", g_no_deps, g_no_provs, &make_undeclared};

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<medulla::runtime> rt =
        std::make_shared<medulla::runtime>(io.get_executor());

    template <typename Fn>
    void run(Fn&& fn) {
        g_log.clear();        struct driver {
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

TEST_CASE("provider mounts and activates") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto spec = h.spec(&g_provider_desc, {{"value", "p1"}});
        auto fh = co_await rt.mount(std::move(spec));
        co_await rt.wait_idle();
        CHECK(rt.state_of(fh.id()) == medulla::fiber_state::active);

        auto root = rt.root_context();
        auto db = root.find<database>(db_key);
        REQUIRE(db);
        CHECK((*db)->name == "p1");
    });
    CHECK(g_log == std::vector<std::string>{"setup:provide:p1", "active:p1"});
}

TEST_CASE("consumer waits for its dependencies") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto consumer = co_await rt.mount(
            h.spec(&g_consumer_desc, {{"tag", "c1"}}));
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::inactive);
        CHECK(rt.error_of(consumer.id()) != nullptr);

        co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::active);
    });
    CHECK(g_log == std::vector<std::string>{"setup:provide:p1", "active:p1",
                                            "consume:c1:p1",
                                            "c-setup:c1"});
}

TEST_CASE("retiring a provider unloads consumers before provider cleanup") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto provider = co_await rt.mount(
            h.spec(&g_provider_desc, {{"value", "p1"}}));
        auto consumer = co_await rt.mount(
            h.spec(&g_consumer_desc, {{"tag", "c1"}}));
        co_await rt.wait_idle();

        co_await rt.retire(provider);
        co_await rt.wait_idle();
        CHECK(rt.state_of(provider.id()) == medulla::fiber_state::inactive);
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::inactive);
    });
    // consumer cleanup (which reads the provider lease) runs before the
    // provider's own cleanup
    auto c_cleanup = std::find(g_log.begin(), g_log.end(),
                               "c-cleanup:c1:p1");
    auto p_cleanup = std::find(g_log.begin(), g_log.end(),
                               "cleanup:provide:p1");
    REQUIRE(c_cleanup != g_log.end());
    REQUIRE(p_cleanup != g_log.end());
    CHECK(c_cleanup < p_cleanup);
}

TEST_CASE("provider replacement reactivates consumers") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto p1 = co_await rt.mount(
            h.spec(&g_provider_desc, {{"value", "p1"}}));
        auto consumer = co_await rt.mount(
            h.spec(&g_consumer_desc, {{"tag", "c1"}}));
        co_await rt.wait_idle();

        co_await rt.retire(p1);
        auto p2 = co_await rt.mount(
            h.spec(&g_provider_desc, {{"value", "p2"}}));
        co_await rt.wait_idle();

        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::active);
        CHECK(rt.state_of(p2.id()) == medulla::fiber_state::active);
    });
    CHECK(std::find(g_log.begin(), g_log.end(), "consume:c1:p2") !=
          g_log.end());
    CHECK(std::find(g_log.begin(), g_log.end(), "consume:c1:p1") !=
          g_log.end());
}

TEST_CASE("consumers mounted while a provider is retiring stay inactive") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto p1 = co_await rt.mount(
            h.spec(&g_provider_desc, {{"value", "p1"}}));
        co_await rt.wait_idle();

        auto ret = rt.retire(p1);
        auto late = co_await rt.mount(
            h.spec(&g_consumer_desc, {{"tag", "late"}}));
        co_await std::move(ret);
        co_await rt.wait_idle();
        CHECK(rt.state_of(late.id()) == medulla::fiber_state::inactive);

        co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p2"}}));
        co_await rt.wait_idle();
        CHECK(rt.state_of(late.id()) == medulla::fiber_state::active);
    });
    CHECK(std::find(g_log.begin(), g_log.end(), "consume:late:p2") !=
          g_log.end());
}

TEST_CASE("undeclared capability access is an activation error") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        auto bad = co_await rt.mount(h.spec(&g_undeclared_desc));
        co_await rt.wait_idle();
        CHECK(rt.state_of(bad.id()) == medulla::fiber_state::inactive);
        CHECK(rt.error_of(bad.id()) != nullptr);
    });
}

TEST_CASE("optional dependencies allow activation without a provider") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto consumer = co_await rt.mount(
            h.spec(&g_optional_consumer_desc));
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::active);
        CHECK(rt.error_of(consumer.id()) == nullptr);

        co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::active);
    });
    CHECK(std::find(g_log.begin(), g_log.end(), "opt:none") != g_log.end());
    CHECK(std::find(g_log.begin(), g_log.end(), "opt:p1") != g_log.end());
}

TEST_CASE("retiring while loading never publishes the fiber") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto slow = co_await rt.mount(h.spec(&g_slow_provider_desc));
        auto ret = rt.retire(slow);
        co_await std::move(ret);
        co_await rt.wait_idle();

        CHECK(rt.state_of(slow.id()) == medulla::fiber_state::inactive);
        auto root = rt.root_context();
        CHECK_FALSE(root.find<database>(db_key));
    });
    CHECK(std::find(g_log.begin(), g_log.end(), "slow:active") ==
          g_log.end());
}

TEST_CASE("plugin failure cleans partial effects and stays inactive") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto broken = co_await rt.mount(h.spec(&g_broken_desc));
        co_await rt.wait_idle();
        CHECK(rt.state_of(broken.id()) == medulla::fiber_state::inactive);
        CHECK(rt.error_of(broken.id()) != nullptr);
    });
    CHECK(g_log == std::vector<std::string>{"setup:broken",
                                            "cleanup:broken"});
}

TEST_CASE("retire is idempotent and tolerates unknown handles") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto provider = co_await rt.mount(
            h.spec(&g_provider_desc, {{"value", "p1"}}));
        co_await rt.wait_idle();

        medulla::fiber_handle unknown;
        co_await rt.retire(unknown);

        co_await rt.retire(provider);
        co_await rt.retire(provider);
        co_await rt.wait_idle();
        CHECK(rt.state_of(provider.id()) == medulla::fiber_state::inactive);
    });
}

TEST_CASE("root context provides services to consumers") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto root = rt.root_context();
        root.provide(db_key, std::make_shared<database>("rootdb"));

        auto consumer = co_await rt.mount(
            h.spec(&g_consumer_desc, {{"tag", "c1"}}));
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::active);
    });
    CHECK(std::find(g_log.begin(), g_log.end(), "consume:c1:rootdb") !=
          g_log.end());
}

TEST_CASE("handle cancel reaches a reactivated fiber") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto p1 = co_await rt.mount(
            h.spec(&g_provider_desc, {{"value", "p1"}}));
        auto consumer = co_await rt.mount(
            h.spec(&g_consumer_desc, {{"tag", "c1"}}));
        co_await rt.wait_idle();

        co_await rt.retire(p1);
        co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p2"}}));
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::active);

        auto ret = rt.retire(consumer);
        consumer.cancel();
        co_await std::move(ret);
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == medulla::fiber_state::inactive);
    });
}

TEST_CASE("two consumers drain before their shared provider unloads") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto p = co_await rt.mount(
            h.spec(&g_provider_desc, {{"value", "p1"}}));
        auto c1 = co_await rt.mount(h.spec(&g_consumer_desc, {{"tag", "a"}}));
        auto c2 = co_await rt.mount(h.spec(&g_consumer_desc, {{"tag", "b"}}));
        co_await rt.wait_idle();

        co_await rt.retire(p);
        co_await rt.wait_idle();
        CHECK(rt.state_of(c1.id()) == medulla::fiber_state::inactive);
        CHECK(rt.state_of(c2.id()) == medulla::fiber_state::inactive);
    });
    auto c1_cleanup = std::find(g_log.begin(), g_log.end(),
                                "c-cleanup:a:p1");
    auto c2_cleanup = std::find(g_log.begin(), g_log.end(),
                                "c-cleanup:b:p1");
    auto p_cleanup = std::find(g_log.begin(), g_log.end(),
                               "cleanup:provide:p1");
    REQUIRE(c1_cleanup != g_log.end());
    REQUIRE(c2_cleanup != g_log.end());
    REQUIRE(p_cleanup != g_log.end());
    CHECK(c1_cleanup < p_cleanup);
    CHECK(c2_cleanup < p_cleanup);
}
