// araya_bench — the profiling bench and example host application.
//
// Araya is a static library and its first-party plugins are libraries,
// not entrypoints: every deployment needs a host — a main() that owns the
// io_context, constructs the runtime, and issues the first mounts. The
// host is the paper's orchestrator (Insert/Retire, p. 34): it is
// deliberately not a plugin, because the calculus has an irreducible
// outside that boots the engine and drives the strand.
//
// Everything the host wants to be *application logic*, on the other hand,
// should be a component. The "steady" workload below demonstrates the
// entrypoint-as-plugin pattern: main() mounts exactly one app component,
// and that component assembles its own subgraph through ctx.mount(...),
// so unloading the app cascades to its children (Theorem 73).
//
// The workloads emit the same action vocabulary the TLA+ model checks
// (proof/tla/MC.tla) — mount, retire, replace, reconcile — so bench
// traffic and model traffic are comparable. This is the reference load
// generator for engine profiling; see bench/README.md for the runbook.

#include "araya/logger/logger.hpp"
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"
#include "araya/runtime.hpp"
#include "araya/service.hpp"
#include "araya/task.hpp"
#include "araya/timer/timer.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using araya::component_spec;
using araya::fiber_handle;
using araya::plugin_config;
using araya::plugin_descriptor;
using araya::runtime;
using araya::task;

using clock_type = std::chrono::steady_clock;

struct result {
    std::string name;
    long long ops = 0;
    double wall_ms = 0.0;
    std::size_t fibers = 0;

    double per_op_ns() const {
        return ops ? wall_ms * 1e6 / static_cast<double>(ops) : 0.0;
    }
};

double ms_since(clock_type::time_point t0) {
    return std::chrono::duration<double, std::milli>(clock_type::now() -
                                                     t0)
        .count();
}

// Descriptors are handed around as shared_ptr<plugin_descriptor>, but the
// pool below owns them statically; this borrows one without taking
// ownership.
std::shared_ptr<plugin_descriptor> shared_desc(plugin_descriptor const& d) {
    return std::shared_ptr<plugin_descriptor>(
        const_cast<plugin_descriptor*>(&d), [](plugin_descriptor*) {});
}

// ---------------------------------------------------------------------
// The component pool: the same P/C/O/S shapes the conformance universe
// uses (tests/model_test.cpp). Declarations live in the descriptors;
// apply() only does the effectful work.
// ---------------------------------------------------------------------

struct database {
    std::string name;
};

inline constexpr araya::service_key<database> db_key{"example.db", 1};

struct provider_plugin : araya::plugin {
    std::string tag = "p";

    // provide() publishes a shared service under the key this component
    // declared; the engine tracks it as an effect, so unloading the
    // fiber unbinds it.
    task<void> apply(araya::plugin_context& ctx) override {
        ctx.provide(db_key, std::make_shared<database>(tag));
        co_return;
    }
};

struct consumer_plugin : araya::plugin {
    // require() throws resolution_error when the declared key is
    // unsatisfiable; the engine retires the fiber with that error.
    task<void> apply(araya::plugin_context& ctx) override {
        (void)ctx.require<database>(db_key);
        co_return;
    }
};

struct optional_plugin : araya::plugin {
    // find() is the optional counterpart: it yields nullopt when the key
    // is absent and never fails the fiber.
    task<void> apply(araya::plugin_context& ctx) override {
        (void)ctx.find<database>(db_key);
        co_return;
    }
};

struct self_plugin : araya::plugin {
    task<void> apply(araya::plugin_context& ctx) override {
        (void)ctx.find<database>(db_key);
        ctx.provide(db_key, std::make_shared<database>("self"));
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_provider(plugin_config const& cfg) {
    auto p = std::make_unique<provider_plugin>();
    if (auto it = cfg.find("tag"); it != cfg.end())
        p->tag = it->second;
    return p;
}

std::unique_ptr<araya::plugin> make_consumer(plugin_config const&) {
    return std::make_unique<consumer_plugin>();
}

std::unique_ptr<araya::plugin> make_optional(plugin_config const&) {
    return std::make_unique<optional_plugin>();
}

std::unique_ptr<araya::plugin> make_self(plugin_config const&) {
    return std::make_unique<self_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_db_dep[]{
    {araya::service_id{"example.db", 1}, true, {}}};
static const araya::dependency_spec g_db_opt[]{
    {araya::service_id{"example.db", 1}, false, {}}};
static const araya::provision_spec g_db_prov[]{
    {araya::service_id{"example.db", 1}}};

static const plugin_descriptor g_desc_P{"P", g_no_deps, g_db_prov,
                                        &make_provider};
static const plugin_descriptor g_desc_C{"C", g_db_dep, g_no_provs,
                                        &make_consumer};
static const plugin_descriptor g_desc_O{"O", g_db_opt, g_no_provs,
                                        &make_optional};
static const plugin_descriptor g_desc_S{"S", g_db_opt, g_db_prov,
                                        &make_self};

component_spec provider_spec(std::string tag) {
    plugin_config cfg{{"tag", std::move(tag)}};
    return {shared_desc(g_desc_P), std::move(cfg), nullptr, "P", {}};
}

component_spec consumer_spec() {
    return {shared_desc(g_desc_C), {}, nullptr, "C", {}};
}

// ---------------------------------------------------------------------
// Cascade workload: a dynamically-built dependency chain. Level i
// requires k_{i+1} and provides k_i; the terminal provider provides
// k_{depth} and the head consumer requires k_0. The descriptors are
// built at runtime because the keys are.
// ---------------------------------------------------------------------

struct token {
    std::string name;
};

struct chain_set;

// The chain plugins' apply() bodies need the complete chain_set type, so
// they are defined below its definition.
struct chain_plugin : araya::plugin {
    chain_plugin(chain_set const* s, int l) : set(s), level(l) {}
    chain_set const* set;
    int level;

    task<void> apply(araya::plugin_context& ctx) override;
};

struct head_plugin : araya::plugin {
    explicit head_plugin(chain_set const* s) : set(s) {}
    chain_set const* set;

    task<void> apply(araya::plugin_context& ctx) override;
};

struct tail_plugin : araya::plugin {
    explicit tail_plugin(chain_set const* s) : set(s) {}
    chain_set const* set;

    task<void> apply(araya::plugin_context& ctx) override;
};

// chain_set owns every byte the descriptors reference: names, the
// per-level dependency/provision storage, and the descriptors
// themselves. Construct it once and it stays address-stable.
struct chain_set {
    explicit chain_set(int depth)
        : depth(depth),
          names(depth + 1),
          deps(depth),
          provs(depth),
          descs(depth) {
        for (int i = 0; i <= depth; ++i)
            names[i] = "bench.k" + std::to_string(i);
        // All name storage is final above; the descriptors below hold
        // string_views into names[i], so nothing may reallocate names.
        for (int i = 0; i < depth; ++i) {
            deps[i] = araya::dependency_spec{
                araya::service_id{names[i + 1], 1}, true, {}};
            provs[i] =
                araya::provision_spec{araya::service_id{names[i], 1}};
            descs[i] = plugin_descriptor{
                "chain." + std::to_string(i),
                std::span<araya::dependency_spec const>(&deps[i], 1),
                std::span<araya::provision_spec const>(&provs[i], 1),
                [this, i](plugin_config const&) {
                    return std::make_unique<chain_plugin>(this, i);
                }};
        }
        head_dep = araya::dependency_spec{
            araya::service_id{names[0], 1}, true, {}};
        tail_prov = araya::provision_spec{
            araya::service_id{names[depth], 1}};
        head = plugin_descriptor{
            "head", std::span<araya::dependency_spec const>(&head_dep, 1),
            g_no_provs, [this](plugin_config const&) {
                return std::make_unique<head_plugin>(this);
            }};
        tail = plugin_descriptor{
            "tail", g_no_deps,
            std::span<araya::provision_spec const>(&tail_prov, 1),
            [this](plugin_config const&) {
                return std::make_unique<tail_plugin>(this);
            }};
    }

    araya::service_key<token> key(int i) const { return {names[i], 1}; }

    int depth;
    std::vector<std::string> names;
    std::vector<araya::dependency_spec> deps;
    std::vector<araya::provision_spec> provs;
    std::vector<plugin_descriptor> descs;
    araya::dependency_spec head_dep;
    araya::provision_spec tail_prov;
    plugin_descriptor head;
    plugin_descriptor tail;
};

task<void> chain_plugin::apply(araya::plugin_context& ctx) {
    (void)ctx.require<token>(set->key(level + 1));
    ctx.provide(set->key(level),
                std::make_shared<token>(set->names[level]));
    co_return;
}

task<void> head_plugin::apply(araya::plugin_context& ctx) {
    (void)ctx.require<token>(set->key(0));
    co_return;
}

task<void> tail_plugin::apply(araya::plugin_context& ctx) {
    ctx.provide(set->key(set->depth),
                std::make_shared<token>(set->names[set->depth]));
    co_return;
}

// ---------------------------------------------------------------------
// Steady workload: the entrypoint-as-plugin pattern. main() mounts one
// app component; the app mounts ticker children through ctx.mount(...)
// (tracked effects, so retiring the app retires the tickers), and each
// ticker drives a disposal-aware interval on the timer service.
// ---------------------------------------------------------------------

inline std::atomic<long long> g_ticks{0};

struct ticker_plugin : araya::plugin {
    task<void> apply(araya::plugin_context& ctx) override {
        // Both services are declared in the descriptor, so they are
        // resolved before apply() runs.
        auto log = ctx.require<araya::logger::logger_service>(
            araya::logger::logger_key);
        auto timer = ctx.require<araya::timer::timer_service>(
            araya::timer::timer_key);
        auto logger = log->named("bench.ticker");
        // interval() is a tracked effect: unloading the ticker cancels
        // the timer. Dropping the registration does not.
        timer->interval(ctx, std::chrono::milliseconds(10),
                        [logger]() mutable {
                            logger.debug(
                                "tick {}", g_ticks.fetch_add(
                                              1, std::memory_order_relaxed) +
                                              1);
                            return true;
                        });
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_ticker(plugin_config const&) {
    return std::make_unique<ticker_plugin>();
}

static const araya::dependency_spec g_ticker_deps[]{
    {araya::service_id{"logger", 1}, true, {}},
    {araya::service_id{"timer", 1}, true, {}}};

static const plugin_descriptor g_ticker_desc{
    "ticker", std::span<araya::dependency_spec const>(g_ticker_deps),
    g_no_provs, &make_ticker};

struct app_plugin : araya::plugin {
    int children = 0;

    task<void> apply(araya::plugin_context& ctx) override {
        for (int i = 0; i < children; ++i)
            co_await ctx.mount(component_spec{
                shared_desc(g_ticker_desc), {}, nullptr,
                "ticker." + std::to_string(i), {}});
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_app(plugin_config const& cfg) {
    auto p = std::make_unique<app_plugin>();
    if (auto it = cfg.find("children"); it != cfg.end())
        p->children = std::max(0, std::stoi(it->second));
    return p;
}

static const plugin_descriptor g_app_desc{"app", g_no_deps, g_no_provs,
                                          &make_app};

// ---------------------------------------------------------------------
// Workload drivers. Each one plays the orchestrator against the runtime
// and reports a machine-readable summary line.
// ---------------------------------------------------------------------

// mount: a single provider, then repeated mount+retire of required
// consumers — activation, guard accounting, and unload churn.
task<result> workload_mount(runtime& rt, long long iterations) {
    auto p = co_await rt.mount(provider_spec("p"));
    co_await rt.wait_idle();
    auto t0 = clock_type::now();
    for (long long i = 0; i < iterations; ++i) {
        auto h = co_await rt.mount(consumer_spec());
        co_await rt.retire(h);
    }
    co_await rt.wait_idle();
    double wall = ms_since(t0);
    co_await rt.retire(p);
    co_await rt.wait_idle();
    co_return result{"mount", iterations, wall, rt.fiber_count()};
}

// cascade: build the chain deepest-first (every fiber parks on the
// guard), then insert the terminal provider and let the apply cascade
// run up; retiring the provider tears the whole chain down with it.
task<result> workload_cascade(runtime& rt, long long iterations,
                              int depth) {
    chain_set set(depth);
    auto t0 = clock_type::now();
    for (long long it = 0; it < iterations; ++it) {
        auto hd = co_await rt.mount(
            component_spec{shared_desc(set.head), {}, nullptr,
                           "head", {}});
        std::vector<fiber_handle> mids(depth);
        for (int i = depth - 1; i >= 0; --i)
            mids[i] = co_await rt.mount(
                component_spec{shared_desc(set.descs[i]), {}, nullptr,
                               "chain." + std::to_string(i), {}});
        auto tl = co_await rt.mount(
            component_spec{shared_desc(set.tail), {}, nullptr,
                           "tail", {}});
        co_await rt.wait_idle();
        co_await rt.retire(tl);
        co_await rt.wait_idle();
        co_await rt.retire(hd);
        for (auto& h : mids)
            co_await rt.retire(h);
        co_await rt.wait_idle();
    }
    double wall = ms_since(t0);
    co_return result{"cascade", iterations, wall,
                     rt.fiber_count()};
}

// replace: provider replacement storms under the paper's single-source
// discipline — retire the old provider, then insert the new one, so the
// binding, the notify fan-out, and the guard re-dispatch run per swap.
task<result> workload_replace(runtime& rt, long long iterations) {
    auto cur = co_await rt.mount(provider_spec("a"));
    co_await rt.wait_idle();
    bool flip = true;
    auto t0 = clock_type::now();
    for (long long i = 0; i < iterations; ++i) {
        co_await rt.retire(cur);
        cur = co_await rt.mount(provider_spec(flip ? "b" : "a"));
        flip = !flip;
        co_await rt.wait_idle();
    }
    double wall = ms_since(t0);
    co_await rt.retire(cur);
    co_await rt.wait_idle();
    co_return result{"replace", iterations, wall,
                     rt.fiber_count()};
}

// reconcile: random desired-tree diffs (the declarative host interface)
// over a fixed seed, so runs are comparable.
task<result> workload_reconcile(runtime& rt, long long iterations) {
    std::mt19937 rng{0x5eed};
    auto t0 = clock_type::now();
    for (long long i = 0; i < iterations; ++i) {
        std::vector<araya::desired_component> desired;
        bool has_p = rng() % 10 < 9;
        bool has_o = rng() % 2;
        bool has_s = !has_p && rng() % 2;
        if (!has_p && !has_s)
            has_s = true;
        if (has_p)
            desired.push_back(
                {"p", {shared_desc(g_desc_P), {}, nullptr, "P", {}}});
        if (has_o)
            desired.push_back(
                {"o", {shared_desc(g_desc_O), {}, nullptr, "O", {}}});
        if (has_s)
            desired.push_back(
                {"s", {shared_desc(g_desc_S), {}, nullptr, "S", {}}});
        co_await rt.reconcile(std::move(desired));
        co_await rt.wait_idle();
    }
    co_await rt.reconcile({});
    co_await rt.wait_idle();
    double wall = ms_since(t0);
    co_return result{"reconcile", iterations, wall,
                     rt.fiber_count()};
}

// steady: one app component assembles its ticker subgraph; the host
// merely waits, then retires the app and the cascade does the rest.
task<result> workload_steady(runtime& rt, int children, long long steady_ms,
                             araya::timer::timer_service& timer) {
    g_ticks.store(0);
    plugin_config cfg{{"children", std::to_string(children)}};
    auto app = co_await rt.mount(component_spec{
        shared_desc(g_app_desc), std::move(cfg), nullptr, "app", {}});
    co_await rt.wait_idle();
    auto t0 = clock_type::now();
    co_await timer.sleep(std::chrono::milliseconds(steady_ms));
    double wall = ms_since(t0);
    co_await rt.retire(app);
    co_await rt.wait_idle();
    co_return result{"steady", g_ticks.load(), wall,
                     rt.fiber_count()};
}

// ---------------------------------------------------------------------
// The host driver: mounts the infra plugins, runs the workloads, and
// proves every run quiesced with invariants intact.
// ---------------------------------------------------------------------

task<void> bench_main(runtime& rt, std::string const& workload_name,
                      long long iterations, int components, int depth,
                      long long steady_ms, component_spec logger_spec,
                      component_spec timer_spec) {
    auto h_logger = co_await rt.mount(std::move(logger_spec));
    auto h_timer = co_await rt.mount(std::move(timer_spec));
    co_await rt.wait_idle();

    // The host is not a component; it reaches into the root context for
    // the services the infra plugins just provided.
    auto log = rt.root_context().require<araya::logger::logger_service>(
        araya::logger::logger_key);
    auto timer = rt.root_context().require<araya::timer::timer_service>(
        araya::timer::timer_key);

    std::vector<std::string> workloads;
    if (workload_name == "all")
        workloads = {"mount", "cascade", "replace", "reconcile",
                     "steady"};
    else
        workloads.push_back(workload_name);

    for (auto const& w : workloads) {
        log->root().info("workload '{}' starting", w);
        result r;
        if (w == "mount")
            r = co_await workload_mount(rt, iterations);
        else if (w == "cascade")
            r = co_await workload_cascade(rt, iterations, depth);
        else if (w == "replace")
            r = co_await workload_replace(rt, iterations);
        else if (w == "reconcile")
            r = co_await workload_reconcile(rt, iterations);
        else if (w == "steady")
            r = co_await workload_steady(rt, components, steady_ms,
                                         *timer);
        else {
            log->root().error("unknown workload '{}'", w);
            continue;
        }
        std::printf(
            "workload=%-9s ops=%lld wall_ms=%.3f per_op_ns=%.1f "
            "fibers=%zu\n",
            r.name.c_str(), r.ops, r.wall_ms, r.per_op_ns(), r.fibers);
    }

    // Every workload must quiesce and pass the invariant sweep; a
    // violation aborts here instead of silently skewing the numbers.
    co_await rt.wait_idle();
    co_await rt.validate_invariants_async();
    log->root().info("bench done: {} fibers", rt.fiber_count());

    co_await rt.retire(h_timer);
    co_await rt.retire(h_logger);
    co_await rt.wait_idle();
}

// ---------------------------------------------------------------------
// CLI and bootstrap
// ---------------------------------------------------------------------

struct options {
    std::string workload = "all";
    long long iterations = 1000;
    int components = 32;
    int depth = 4;
    long long steady_ms = 1000;
    std::string log_level = "info";
};

void usage(FILE* out) {
    std::fprintf(
        out,
        "usage: araya_bench [options]\n"
        "  --workload <name|all>   mount, cascade, replace, reconcile, "
        "steady (default all)\n"
        "  --iterations N          ops per workload (default 1000)\n"
        "  --components N          ticker fibers for steady (default 32)\n"
        "  --depth N               cascade chain depth (default 4)\n"
        "  --steady-ms T           steady workload duration (default "
        "1000)\n"
        "  --log-level <l>         error|warn|info|debug (default info)\n"
        "  --help\n");
}

options parse_args(int argc, char** argv) {
    options o;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](char const* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--workload")
            o.workload = next("--workload");
        else if (arg == "--iterations")
            o.iterations = std::stoll(next("--iterations"));
        else if (arg == "--components")
            o.components = std::stoi(next("--components"));
        else if (arg == "--depth")
            o.depth = std::max(1, std::stoi(next("--depth")));
        else if (arg == "--steady-ms")
            o.steady_ms = std::stoll(next("--steady-ms"));
        else if (arg == "--log-level")
            o.log_level = next("--log-level");
        else if (arg == "--help") {
            usage(stdout);
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown option '%s'\n", arg.c_str());
            usage(stderr);
            std::exit(2);
        }
    }
    if (o.iterations < 1)
        o.iterations = 1;
    if (o.components < 0)
        o.components = 0;
    if (o.steady_ms < 1)
        o.steady_ms = 1;
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    auto opt = parse_args(argc, argv);

    // The host's irreducible duties: own the strand, boot the engine,
    // and issue the first causal actions.
    boost::asio::io_context io;
    runtime rt{io.get_executor()};

    // The logger plugin is mounted like any other component; its quill
    // console sink timestamps every record, keeping the bench phases
    // time-ordered. A production host would load plugins by path via
    // module_loader instead of linking them statically.
    plugin_config log_cfg{{"name", "bench"}, {"level", opt.log_level}};
    component_spec logger_spec{
        shared_desc(araya::logger::plugin_descriptor()),
        std::move(log_cfg), nullptr, "logger", {}};
    component_spec timer_spec{
        shared_desc(araya::timer::plugin_descriptor()), {}, nullptr,
        "timer", {}};

    std::exception_ptr run_error;
    boost::asio::co_spawn(
        io.get_executor(),
        bench_main(rt, opt.workload, opt.iterations, opt.components,
                   opt.depth, opt.steady_ms, std::move(logger_spec),
                   std::move(timer_spec)),
        [&run_error](std::exception_ptr ep) { run_error = ep; });
    io.run();
    if (run_error)
        std::rethrow_exception(run_error);
    return 0;
}
