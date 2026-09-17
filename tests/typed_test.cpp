#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/typed.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>
#include <type_traits>

// A commutative service type: the witness marker.
struct database {
    std::string name;
};

// A non-commutative service type: no witness.
struct pipeline {
    std::string name;
};

template <>
inline constexpr bool araya::is_commutative_key_v<database> = true;

inline constexpr araya::service_key<database> db_key{"example.db", 1};
inline constexpr araya::service_key<pipeline> pipe_key{"example.pipe", 1};
inline constexpr araya::service_key<database> other_key{"example.other", 1};

using caps = araya::capabilities<&db_key, &pipe_key>;

// The capability math is what gates the calls: an undeclared key is not in
// the set, so require/provide of it cannot compile. (GCC 14 turns the
// resulting constraint failure into a hard error rather than an
// unsatisfied requires-expression, so the negative cases are asserted on
// the constraint itself.)
static_assert(caps::contains<&db_key>);
static_assert(caps::contains<&pipe_key>);
static_assert(!caps::contains<&other_key>);

static_assert(araya::commutative_key<database>);
static_assert(!araya::commutative_key<pipeline>);

namespace {

struct typed_provider : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        araya::typed_context<caps> typed{ctx};
        typed.provide<&db_key>(std::make_shared<database>("p1"));
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_typed_provider(
    araya::plugin_config const&) {
    return std::make_unique<typed_provider>();
}

struct typed_consumer : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        araya::typed_context<caps> typed{ctx};
        auto db = typed.require<&db_key>();
        REQUIRE(db);
        REQUIRE(db->name == "p1");
        // The lease's provenance is a type-level fact.
        static_assert(std::is_same_v<
                      decltype(db),
                      araya::tagged_lease<database,
                                            araya::committed_tag>>);
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_typed_consumer(
    araya::plugin_config const&) {
    return std::make_unique<typed_consumer>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_db_dep[]{
    {araya::service_id{"example.db", 1}, true}};
static const araya::provision_spec g_db_prov[]{
    {araya::service_id{"example.db", 1}}};

static const araya::plugin_descriptor g_typed_provider_desc{
    "typed-provider", g_no_deps, g_db_prov, &make_typed_provider};
static const araya::plugin_descriptor g_typed_consumer_desc{
    "typed-consumer", g_db_dep, g_no_provs, &make_typed_consumer};

struct harness {
    boost::asio::io_context io;
    std::shared_ptr<araya::runtime> rt =
        std::make_shared<araya::runtime>(io.get_executor());

    template <typename Fn>
    void run(Fn&& fn) {
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
};

}  // namespace

TEST_CASE("typed contexts mediate access end to end") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        co_await rt.mount(h.spec(&g_typed_provider_desc));
        auto consumer = co_await rt.mount(h.spec(&g_typed_consumer_desc));
        co_await rt.wait_idle();
        CHECK(rt.state_of(consumer.id()) == araya::fiber_state::active);
    });
}

TEST_CASE("typed contexts carry lease metadata and reject undeclared "
          "metadata-free access statically") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        rt.root()->set_metadata(db_key.id, {{"mode", "readonly"}});
        co_await rt.mount(h.spec(&g_typed_provider_desc));
        co_await rt.mount(h.spec(&g_typed_consumer_desc));
        co_await rt.wait_idle();
        co_await rt.validate_invariants_async();
    });
}

TEST_CASE("typestate handles consume their transitions") {
    harness h;
    h.run([&](araya::runtime& rt) -> araya::task<void> {
        auto raw = co_await rt.mount(h.spec(&g_typed_provider_desc));
        auto active = araya::typed_handle<araya::fiber_state::active>::
            claim(raw);
        co_await rt.wait_idle();
        CHECK(active.state() == araya::fiber_state::active);

        auto inactive = co_await std::move(active).retire(rt);
        co_await rt.wait_idle();
        CHECK(rt.state_of(inactive.id()) == araya::fiber_state::inactive);
        CHECK(rt.fiber_count() == 0);
    });
}
