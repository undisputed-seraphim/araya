#include <catch2/catch_test_macros.hpp>

#include "medulla/abi.hpp"
#include "medulla/abi_host.hpp"
#include "medulla/plugin.hpp"
#include "medulla/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

struct database {
    std::string name;
};

inline constexpr medulla::service_key<database> db_key{"example.db", 1};

static std::vector<std::string> g_log;
static medulla::runtime* g_validate_rt = nullptr;

// ---- components ----------------------------------------------------------

struct provider_plugin : medulla::plugin {
    std::string value;

    medulla::task<void> apply(medulla::plugin_context& ctx) override {
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
        ctx.effect([tag = tag, shared = db.shared()]() -> medulla::cleanup_action {
            return [tag, shared] {
                g_log.push_back("cleanup:" + tag + ":" + shared->name);
                if (g_validate_rt)
                    g_validate_rt->validate_invariants();
            };
        });
        g_log.push_back("consume:" + tag);
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_consumer(
    medulla::plugin_config const& cfg) {
    auto p = std::make_unique<consumer_plugin>();
    p->tag = cfg.at("tag");
    return p;
}

struct validating_provider_plugin : medulla::plugin {
    std::string value;

    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        ctx.effect([value = value]() -> medulla::cleanup_action {
            return [value] {
                g_log.push_back("p-cleanup:" + value);
                if (g_validate_rt)
                    g_validate_rt->validate_invariants();
            };
        });
        ctx.provide(db_key, std::make_shared<database>(value));
        g_log.push_back("active:" + value);
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_validating_provider(
    medulla::plugin_config const& cfg) {
    auto p = std::make_unique<validating_provider_plugin>();
    p->value = cfg.at("value");
    return p;
}

struct optional_consumer_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        auto db = ctx.find<database>(db_key);
        g_log.push_back(db ? "opt:yes" : "opt:no");
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_optional_consumer(
    medulla::plugin_config const&) {
    return std::make_unique<optional_consumer_plugin>();
}

struct slow_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        auto stopped = co_await medulla::stop_requested();
        if (!stopped) {
            g_log.push_back("slow:active");
            ctx.provide(db_key, std::make_shared<database>("slow"));
        }
    }
};

std::unique_ptr<medulla::plugin> make_slow(medulla::plugin_config const&) {
    return std::make_unique<slow_plugin>();
}

struct broken_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        ctx.effect([tag = std::string("broken")]() -> medulla::cleanup_action {
            g_log.push_back("broken:setup");
            return [] { g_log.push_back("broken:cleanup"); };
        });
        throw std::runtime_error("broken apply");
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_broken(medulla::plugin_config const&) {
    return std::make_unique<broken_plugin>();
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
static const medulla::plugin_descriptor g_validating_provider_desc{
    "validating-provider", g_no_deps, g_db_prov, &make_validating_provider};
static const medulla::plugin_descriptor g_consumer_desc{
    "consumer", g_db_dep, g_no_provs, &make_consumer};
static const medulla::plugin_descriptor g_optional_consumer_desc{
    "optional-consumer", g_db_optional, g_no_provs, &make_optional_consumer};
static const medulla::plugin_descriptor g_slow_desc{
    "slow", g_no_deps, g_db_prov, &make_slow};
static const medulla::plugin_descriptor g_broken_desc{
    "broken", g_no_deps, g_no_provs, &make_broken};

// ---- native module (in-process ABI boundary) ------------------------------

static medulla_instance_v1* make_native_mod(const medulla_config_pair*,
                                            std::size_t) {
    auto* inst = new medulla_instance_v1{};
    inst->apply = +[](medulla_instance_v1* self, medulla_fiber fiber,
                      void (*complete)(medulla_fiber, int, const char*)) {
        (void)self;
        g_log.push_back("native:apply");
        complete(fiber, MEDULLA_ABI_OK, nullptr);
    };
    inst->reconfigure = nullptr;
    inst->destroy = +[](medulla_instance_v1* self) { delete self; };
    return inst;
}

static medulla_plugin_descriptor_v1 g_native_desc{
    MEDULLA_ABI_VERSION, "native", nullptr, 0, nullptr, 0, &make_native_mod};

extern "C" const medulla_plugin_descriptor_v1* invariants_native_entry(
    const medulla_host_api_v1*) {
    return &g_native_desc;
}

// ---- harness ---------------------------------------------------------------

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
        g_validate_rt = nullptr;
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

TEST_CASE("invariants hold at rest and through a provider cascade") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        co_await rt.validate_invariants_async();

        auto p = co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();

        auto c1 = co_await rt.mount(h.spec(&g_consumer_desc, {{"tag", "a"}}));
        auto c2 = co_await rt.mount(h.spec(&g_consumer_desc, {{"tag", "b"}}));
        co_await rt.wait_idle();
        CHECK(rt.state_of(c1.id()) == medulla::fiber_state::active);
        CHECK(rt.state_of(c2.id()) == medulla::fiber_state::active);
        co_await rt.validate_invariants_async();

        co_await rt.retire(c1);
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();

        co_await rt.retire(p);
        co_await rt.wait_idle();
        CHECK(rt.state_of(c2.id()) == medulla::fiber_state::inactive);
        co_await rt.validate_invariants_async();
    });
}

TEST_CASE("invariants hold mid-teardown, from inside plugin cleanup") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        g_validate_rt = &rt;
        auto p = co_await rt.mount(
            h.spec(&g_validating_provider_desc, {{"value", "p1"}}));
        auto c1 = co_await rt.mount(h.spec(&g_consumer_desc, {{"tag", "a"}}));
        auto c2 = co_await rt.mount(h.spec(&g_consumer_desc, {{"tag", "b"}}));
        co_await rt.wait_idle();

        // Consumer cleanup validates the engine while its committed view
        // still names the retiring provider (Theorem 70(ii)); the provider
        // cleanup validates once all consumer edges are drained.
        co_await rt.retire(p);
        co_await rt.wait_idle();
        CHECK(rt.state_of(c1.id()) == medulla::fiber_state::inactive);
        CHECK(rt.state_of(c2.id()) == medulla::fiber_state::inactive);
        g_validate_rt = nullptr;
    });
}

TEST_CASE("invariants hold across an optional dependency flip") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto c = co_await rt.mount(h.spec(&g_optional_consumer_desc));
        co_await rt.wait_idle();
        CHECK(rt.state_of(c.id()) == medulla::fiber_state::active);
        co_await rt.validate_invariants_async();

        auto p = co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();

        co_await rt.retire(p);
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();
    });
    CHECK(std::find(g_log.begin(), g_log.end(), "opt:no") != g_log.end());
    CHECK(std::find(g_log.begin(), g_log.end(), "opt:yes") != g_log.end());
}

TEST_CASE("invariants hold through reconciliation") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        std::vector<medulla::desired_component> desired;
        desired.push_back(medulla::desired_component{
            "p", h.spec(&g_provider_desc, {{"value", "p1"}})});
        desired.push_back(medulla::desired_component{
            "c", h.spec(&g_consumer_desc, {{"tag", "a"}})});
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();

        // Config-only revision: same descriptor, new payload.
        desired.push_back(medulla::desired_component{
            "p", h.spec(&g_provider_desc, {{"value", "p2"}})});
        desired.push_back(medulla::desired_component{
            "c", h.spec(&g_consumer_desc, {{"tag", "a"}})});
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();

        // Removal of everything.
        co_await rt.reconcile({});
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();
    });
}

TEST_CASE("invariants hold for native ABI fibers") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        auto desc = medulla::abi::wrap(&invariants_native_entry);
        auto fh = co_await rt.mount(medulla::component_spec{
            desc, {}, nullptr, ""});
        co_await rt.wait_idle();
        CHECK(rt.state_of(fh.id()) == medulla::fiber_state::active);
        co_await rt.validate_invariants_async();

        co_await rt.retire(fh);
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();
    });
}

TEST_CASE("invariants hold under seeded random lifecycle operations") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        std::mt19937 rng(0x5eed);
        std::map<std::string, medulla::component_spec> desired;
        std::vector<medulla::desired_component> want;
        int next_tag = 0;

        auto sync_desired = [&] {
            want.clear();
            for (auto const& [path, s] : desired)
                want.push_back(medulla::desired_component{path, s});
        };

        for (int i = 0; i < 300; ++i) {
            auto roll = std::uniform_int_distribution<int>(0, 9)(rng);
            if (roll < 3) {
                auto path = "p" + std::to_string(
                    std::uniform_int_distribution<int>(0, 1)(rng));
                if (!desired.contains(path))
                    desired.emplace(path, h.spec(&g_provider_desc,
                        {{"value", path + "-" + std::to_string(i)}}));
            } else if (roll < 4 && !desired.empty()) {
                auto it = desired.begin();
                std::advance(it, std::uniform_int_distribution<std::size_t>(
                    0, desired.size() - 1)(rng));
                desired.erase(it);
            } else if (roll < 6) {
                auto path = "c" + std::to_string(next_tag++);
                desired.emplace(path, h.spec(&g_consumer_desc, {{"tag", path}}));
            } else if (roll < 7) {
                auto path = "o" + std::to_string(next_tag++);
                desired.emplace(path, h.spec(&g_optional_consumer_desc));
            } else if (roll < 9 && !desired.empty()) {
                // Revise one provider's config in place.
                for (auto& [path, s] : desired)
                    if (path.starts_with("p")) {
                        s = h.spec(&g_provider_desc,
                            {{"value", path + "-rev" + std::to_string(i)}});
                        break;
                    }
            }
            sync_desired();
            co_await rt.reconcile(std::move(want));
            co_await rt.wait_idle();
            co_await rt.validate_invariants_async();
        }

        co_await rt.reconcile({});
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();
    });
}
TEST_CASE("invariants hold across degenerate sequences") {
    harness h;
    h.run([&](medulla::runtime& rt) -> medulla::task<void> {
        medulla::fiber_handle unknown;
        co_await rt.retire(unknown);
        co_await rt.validate_invariants_async();

        auto p = co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
        co_await rt.wait_idle();
        co_await rt.retire(p);
        co_await rt.retire(p);
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();

        // Retire while the fiber is still loading.
        auto slow = co_await rt.mount(h.spec(&g_slow_desc));
        auto ret = rt.retire(slow);
        co_await std::move(ret);
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();

        // Activation failure with partial effects.
        auto broken = co_await rt.mount(h.spec(&g_broken_desc));
        co_await rt.wait_idle();
        CHECK(rt.state_of(broken.id()) == medulla::fiber_state::inactive);
        CHECK(rt.error_of(broken.id()) != nullptr);
        co_await rt.validate_invariants_async();

        // Unsatisfied required dependency stays inactive with an error.
        auto hungry = co_await rt.mount(h.spec(&g_consumer_desc, {{"tag", "x"}}));
        co_await rt.wait_idle();
        CHECK(rt.state_of(hungry.id()) == medulla::fiber_state::inactive);
        co_await rt.validate_invariants_async();
    });
    CHECK(std::find(g_log.begin(), g_log.end(), "broken:cleanup") != g_log.end());
    CHECK(std::find(g_log.begin(), g_log.end(), "slow:active") == g_log.end());
}
