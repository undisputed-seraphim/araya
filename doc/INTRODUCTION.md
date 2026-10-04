# Introduction to Araya

Araya is a C++23 runtime for building applications out of **components that can appear,
disappear, or be replaced while the process stays alive**. It is the engine, not an
application: a library plus a small set of contracts that a host program and its components
follow. If you have ever wanted to unload a plugin, swap a service provider, or rewire a
dependency graph at runtime without restarting the process or leaking the effects of the old
code, this is the framework for that.

Araya is an independent C++ realization of the component calculus in *A Programming Paradigm
for Spatiotemporal Composability* (Shi, Zhang, Cui — arXiv:2608.25512). The paper's reference
implementation is Cordis (TypeScript); Araya reuses its semantics and is held to them by a
TLA+ refinement model checker and a conformance suite (see `proof/README.md`). This document
introduces the framework in practical terms. You do not need to read the paper to use Araya,
and you do not need to be a mathematician to understand the guarantees it gives you.

---

## 1. The problem: dynamic composition

Normal composition is static. Function calls, imports, and inheritance are resolved before the
program runs and never change. But real systems increasingly need to load, unload, and
reconfigure functionality at runtime: plugins, hot-reloadable extensions, self-modifying
agent harnesses, service meshes.

The paper names two problems that any such system must solve, and they are orthogonal:

- **Temporal composability** — when a component is removed, everything it did to the shared
  environment must be undone, safely and completely. Every file opened, listener registered,
  service published, resource acquired.
- **Spatial composability** — components must describe what they need from each other, and the
  runtime must connect them as dependencies come and go, activating a component only when its
  needs are met and tearing it down in an order that is safe for everyone involved.

Most systems punt on one or both. They restart the whole process to unload code, or they let
plugins reach each other through ad-hoc global state with no structured contract. Araya takes
both problems seriously and makes the runtime enforce the rules.

The two problems map to two classical ideas from programming-language theory:

- An **effect** is what a computation *does to* its environment. Araya lifts effects into
  **revertible effects**: every change a component makes through the runtime carries an
  inverse, and the runtime keeps that inverse so removal can undo the change.
- A **coeffect** is what a computation *requires from* its environment. Araya lifts coeffects
  into **reactive coeffects**: a component declares the services it needs, and the runtime
  watches the environment and activates or deactivates the component as those services appear
  and disappear.

Araya unifies the two into a single context object, so every operation goes through one place
and is automatically tracked and routed. This is the paper's *context paradigm*.

---

## 2. The core model in five minutes

Araya's whole design rests on four nouns and two mechanics.

### Components and fibers

A **component** is a unit of functionality. It declares:

- **dependencies** — service keys it reads (its coeffect specification),
- **provisions** — service keys it may publish,
- and an **`apply`** body — the code that runs when it activates.

A **fiber** is one live instance of a component. The same component can be instantiated many
times; each instance is a separate fiber with its own state and its own lifecycle:

```
inactive ──▶ loading ──▶ active ──▶ unloading ──▶ inactive
```

- `loading` is `apply` running.
- `active` means `apply` finished; the fiber's provisions are visible and its listeners may
  fire.
- `unloading` is the reverse: the runtime is running the fiber's accumulated cleanup.
- `inactive` is either "never started" or "fully torn down".

A fiber never jumps straight from `active` to `inactive`. It goes through `unloading` so that
dependents (whose cleanup may still need the fiber's services) get a chance to run first.

### Effects: every change records its own undo

Nothing mutates the shared environment directly. Instead it is done through the context, and
the context records a cleanup for each successful change:

```cpp
araya::registration r = ctx.effect([&] {
    install_something();
    return araya::cleanup_action{[&] { remove_something(); }};
});
```

The cleanup is pushed onto the fiber's **effect stack**. When the fiber unloads, the stack runs
**in reverse order** (last in, first out). This is the paper's *accumulator discipline*. The
`registration` returned by the call owns one entry: drop it and cleanup still runs at teardown;
call `release()` to run it early (for example, to unpublish a service before the rest of the
fiber goes away).

`provide` and `on` (described below) are built on this same mechanism, so they are tracked
automatically. That is why a component author usually does not write an uninstall path at all:
if they made the change through the context, the runtime already knows how to undo it.

### Coeffects: declare what you need, get woken when it changes

A component declares its dependencies, then resolves them by key:

```cpp
static constexpr araya::service_key<database> db{"db", 1};

araya::task<void> apply(araya::plugin_context& ctx) {
    auto lease = ctx.require(db);   // throws resolution_error if absent
    auto maybe = ctx.find(db);      // std::nullopt if absent
    ctx.provide(db, my_database);   // throws if 'db' was not declared
    co_return;
}
```

The runtime enforces two rules strictly:

1. **Declare, then use.** `require`/`find` of an undeclared key is a logic error (it throws
   `std::logic_error`). Providing an undeclared key is also an error. A fiber's declarations
   are fixed for its lifetime.
2. **React to the environment.** A fiber that declares a key does not start until the key is
   *provided by an active fiber*. If the provider later leaves, the runtime deactivates the
   dependent before the provider's own cleanup runs.

Resolution records the **identity of the provider fiber**, not just its value. This is
important: replacing a provider (even with one that publishes an equal value) re-evaluates the
provider's consumers, so no consumer silently keeps a stale binding.

### The runtime: the orchestrator's tool

The `araya::runtime` owns all fibers, the control strand, and the event bus. The host uses it
to drive composition:

```cpp
auto rt = std::make_shared<araya::runtime>(io.get_executor());

auto h = co_await rt->mount(spec);        // install one component, wait until active
co_await rt->retire(h);                    // deactivate, ordered withdrawal
co_await rt->reconcile(desired_tree);      // declarative "make it look like this"
co_await rt->wait_idle();                  // wait until every fiber has settled
```

`reconcile` is the declarative surface: hand it the list of components you want, keyed by a
stable path, and it diffs against what is live — retaining, mounting, retiring, replacing, or
applying an in-place config update as needed.

All composition runs on the runtime's single **control strand** (a Boost.Asio strand). The
awaitable entry points hop onto it for you. Component `apply` bodies run on the strand too;
offload real work to your own executors and return to the strand before touching the context.

---

## 3. What the framework guarantees you

This is the practical payoff. The paper proves a handful of theorems about the calculus; Araya
implements the calculus and is checked against a model of it. In plain terms, here is what you
get.

### 3.1 Removal is exact (temporal composability)

**Recovery exactness** (the paper's Theorem 68): when a fiber's cleanup runs, it removes
exactly *that fiber's* contribution and nothing else — no matter what other fibers did in
between. **Terminal recovery** (Corollary 69): when teardown finishes, the fiber's published
bindings are all gone, so the entry can be safely dropped.

*Why you care:* unloading a component never corrupts unrelated components, even under
concurrent load. Cleanup is not best-effort; it is the exact inverse of what was installed.

*The precise scope:* recovery restores the *binding at each declared key*, up to the
equivalence that key's operations define. A monotone counter that was incremented is not
"un-incremented" unless the key's operations say so; a message already sent stays sent. This
is the *system boundary*: a location is inside the boundary when the system can restore it,
and outside otherwise. Operations that reach outside (a network send, a write to a file other
programs also use) are *emissions*, not tracked effects. If you need to compensate for an
emission, do it with a tracked cleanup that performs the compensating action.

### 3.2 Teardown is ordered (spatial composability)

**Ordering** (Theorem 70): a fiber only starts a transition when its declared dependencies are
actually provided; and a provider is not withdrawn until every dependents that resolved one of
its keys has deactivated. A consumer therefore keeps reading the same dependency *throughout
its own teardown* — closing a connection pool can still hand connections back to the thing
that provided them.

*Why you care:* you can write cleanup code that uses the services your component was built on.
You do not need to defensively null-check dependencies that are "about to disappear".

**Resolution coherence** (Theorem 71): while a fiber is activating, it runs against a single,
stable resolution of its dependencies. If a dependency changes mid-activation, the fiber is
diverted and deactivates rather than completing against a stale view.

*Why you care:* a component never half-initializes against dependencies that changed under it.

### 3.3 The system always settles (progress)

**Progress** (Theorem 73): under three mild conditions — the dependency precedence graph is
acyclic, each activation is finite, and the set of fiber names is finite — the system never
deadlocks and reaches **quiescence** (no transition in progress). `runtime::wait_idle()` is a
direct expression of that guarantee: after any sequence of mount/retire/reconcile operations,
it returns.

*Why you care:* your orchestration loop cannot hang. A dependency cycle is not a deadlock you
discover at runtime; it is a property of the declarations, and Araya reports it up front (see
§3.6).

### 3.4 History does not matter (confluence)

**Confluence** (Theorem 80): whatever sequence of loads, unloads, provider swaps, and
reconfigurations a system has been through, the state it settles at is the same as if the same
final set of components had been assembled once, statically, in dependency order.

*Why you care:* you may reason about a running system by looking at the **quiescent state**
alone. Hot-reload, incremental `reconcile`, and repeated replacement all converge to the
statically-correct composition. This is what makes declarative reconciliation sound.

### 3.5 Components do not step on each other (independence)

**Pairwise independence** (Lemma 66): the effects of distinct components commute — they can be
reordered without changing the outcome — unless they are *entangled* (one provides a key the
other declares). When they are entangled, the ordering rules of §3.2 take over. The
commutativity itself is supplied by the provider as a property of the key's operations, so
two unrelated components can interleave freely.

*Why you care:* the runtime can schedule independent work in any order, which is what makes
the engine parallelizable and the semantics predictable.

### 3.6 Structural problems are diagnosed, not discovered

A dependency **cycle** and a **provider conflict** (two fibers providing the same key in the
same scope) are predictable from declarations alone. Araya's `runtime::on_diagnostic` reports
them when components are loaded, rather than letting them manifest as a mysterious hang. Each
distinct conflict or cycle is reported once.

Note the deliberate design choice: Araya *diagnoses* these but does not hard-prevent them. A
self-provisioning component (one that declares a key it also provides) is reported, and the
single-source discipline the paper assumes for its confluence proof is not enforced at the
engine boundary. The upshot is that you should treat a reported cycle as a bug in your
component graph.

### 3.7 The guarantees do not cover arbitrary physical state

The calculus is explicit about its boundary. Only state that a component reifies as a key — a
service, a tracked effect — is inside the model. Physical state that the system cannot
exclusively control is outside it. The practical rule: **if you want removal to undo it, make
it a tracked effect.** Everything else is your responsibility (or a compensation).

---

## 4. Public interface

The public surface is `include/araya/`. Plugins include a handful of headers; hosts include
`runtime.hpp` and friends. Everything below is real, current API.

### 4.1 The component contract — `araya/plugin.hpp`

```cpp
class plugin {
public:
    virtual ~plugin() = default;
    virtual boost::asio::awaitable<void> apply(plugin_context&) = 0;
    virtual bool reconfigure(plugin_config const&) { return false; }
};

struct plugin_descriptor {                 // static description of a component type
    std::string_view name;
    std::span<dependency_spec const> inject;   // declared dependencies
    std::span<provision_spec const> provide;   // declared provisions
    std::function<std::unique_ptr<plugin>(plugin_config const&)> create;
    std::span<config_field const> config_schema{};
};

struct component_spec {                    // one requested instance
    std::shared_ptr<plugin_descriptor> descriptor;
    plugin_config config;                  // std::map<std::string,std::string>
    std::shared_ptr<context> parent;
    std::string name;
    std::map<std::string, std::string> isolate;   // per-key realms
};
```

- `dependency_spec { service_id key; bool required; service_metadata metadata; }` — a declared
  need. `required = false` makes it optional.
- `provision_spec { service_id key; }` — a declared provision.
- `reconfigure` is the optional **in-place config update**. Return `false` to decline, and the
  runtime reloads the fiber (full deactivate/reactivate) with the new config instead.
- `component_spec::isolate` maps key names to *realm* tags. Two fibers that share a tag share
  the binding; different tags isolate them. This is how several providers of one key coexist in
  one process.

Descriptors are shared and long-lived: the name and the spans must point at storage that
outlives every fiber made from them (static storage in practice).

### 4.2 The activation context — `araya/plugin_context.hpp`

`plugin_context` is the plugin-facing half of one activation. It is a **view**, valid only
while that activation runs (inside `apply`, and inside the listeners and cleanups that
activation registered). Never store it.

| Call | Purpose |
|---|---|
| `require(key)` | Resolve a declared service; throws `resolution_error` if absent. |
| `find(key)` | Optional variant; returns `std::nullopt` if absent or unavailable. |
| `provide(key, service, check = {})` | Publish a declared service. Returns a `provision_handle`. |
| `set_available(key, true)` | Promote an unavailable provision to available (idempotent). |
| `effect(setup)` | Run `setup` now, register its returned cleanup for reverse-order teardown. |
| `on(key, fn, opts)` | Register an event listener owned by this activation. |
| `mount(spec)` | Instantiate a child component as a tracked effect. |
| `scope()`, `root()` | The fiber's scope, and the runtime root. |
| `stop_token()` | Cooperative cancellation for `apply`. |
| `executor()` | The control-strand executor. |

**Availability** deserves a note. `provide` takes an optional check predicate that runs
*once*, at provide-time. A failing or throwing check publishes the binding as **unavailable**.
An unavailable binding reads as absent: `require` parks the consumer, `find` returns nullopt.
Later, the provider promotes it:

```cpp
ctx.set_available(db, true);   // the only transition; idempotent; must run on the strand
```

There is deliberately **no** `set_available(db, false)`: the paper's lifecycle has no edge from
active back to loading, so demotion is not expressible. To withdraw a service, unload its
provider — `runtime::retire`. (Availability is a tier-2 extension Araya adds; it is verified
as a refinement of the paper's lifecycle, see `proof/README.md`.)

**Ordered withdrawal** is what makes teardown safe. When a provider leaves, the runtime first
stops handing it new consumers; already-committed consumers tear down *while still holding
their provider's bindings*; the provider's own cleanup runs only after every consumer has
reached inactive.

**`apply` is cooperative.** Watch `stop_token()` and return promptly when it fires.
`fiber_handle::cancel()` is a stop *request*, never preemption.

### 4.3 Services and leases — `araya/service.hpp`

- `service_id { std::string_view name; uint32_t version; }` — erased identity.
- `service_key<T> { service_id id; }` — a typed key; `T` binds the value type so wrong-typed
  access does not compile. Version defaults to 1.
- `service_lease<T>` — what `require`/`find` return. It holds a `shared_ptr` to the value plus
  the providing fiber's id and merged metadata.

The lifetime rule is worth internalizing: **a lease keeps the value alive past `apply`**, so a
provider cannot unload the object out from under you. But the *binding* is not retained: after
the provider unloads, the same key may be provided by a different fiber. Hold a lease for the
object; call `require` again for the current resolution.

`provision_handle<T>` (what `provide` returns) carries a live copy of the published value.
Capture `handle.value` in listeners you register afterwards, not the temporary you passed to
`provide` — the classic move-then-capture null-pointer trap is avoided by construction.

### 4.4 Events — `araya/events.hpp`

Events are typed and keyed, and the **dispatch mode is part of the key's type**, which fixes
the listener signature:

| Mode | Listener shape | Flow |
|---|---|---|
| `emit` | `fn(Message)` | fire-and-forget; errors go to the diagnostic sink |
| `parallel` | `fn(Message)` | all listeners awaited together; first failure rethrown |
| `serial` | `fn(Message)` | one after another in registration order; first failure aborts |
| `waterfall` | `fn(Message, continuation) -> Message` | chained pipeline; each may transform |
| `bail` | `fn(Message) -> bool` | short-circuits on first `true` |

```cpp
static constexpr araya::event_key<std::string, araya::dispatch_mode::serial>
    files{"files.changed"};

araya::registration r = ctx.on(files, [](std::string const& path) -> araya::task<void> {
    co_await rescan(path);
});
```

Listeners belong to the registering fiber and are removed at its teardown, or earlier via the
returned `registration`. `listener_options` support `prepend`, `once`, `global`, and `scope`
(deliver only to dispatches carrying a matching realm label). Registration and strand-bound
dispatch must run on the control strand; `submit`/`submit_nowait` are the multi-producer
ingress that any thread may call to hand a dispatch to the strand.

### 4.5 The runtime — `araya/runtime.hpp`

`runtime` is the plugin manager, the control strand, and the sole owner of fibers, scopes, and
the event bus. One runtime per Asio executor in practice.

| Member | Purpose |
|---|---|
| `mount(spec)` → `awaitable<fiber_handle>` | Install one component; completes when active. |
| `retire(handle)` → `awaitable<void>` | Ordered deactivation; idempotent. |
| `reconcile(desired)` → `awaitable<void>` | Declarative diff: retain/mount/retire/replace/update. |
| `wait_idle()` → `awaitable<void>` | Quiescence: no transition in progress. |
| `run_on_strand(fn)` | Escape hatch from any thread or coroutine. |
| `bus()` | The `event_bus`. |
| `root()` / `root_context()` | The root scope. |
| `fibers()` / `fibers_async()` | Snapshot of every mounted fiber. |
| `state_of(id)` / `error_of(id)` | Lifecycle state and terminal error of a fiber. |
| `on_diagnostic(sink)` | Register a sink for conflict/cycle reports. |
| `mount_locked(spec, parent)` | Synchronous mount; must be on the strand. |
| `retire_child(id)` | Retire an instantiated child. |
| `validate_invariants()` / `_async()` | Check the engine's internal invariants. |

`fiber_handle` is a cheap, copyable value naming one live activation. It does not keep the
fiber alive. Its `state()` and `error()` remain readable after the fiber's record is erased:
the terminal outcome is published lock-free into a shared cell (`fiber_handle.hpp`).

`fiber_state` is one of `inactive`, `loading`, `active`, `unloading`. A diagnostic snapshot
(`fiber_info`) exposes a fiber's declared inject/provide keys, its committed view, its parent,
and its error — useful for introspection and debugging UIs.

### 4.6 Typed configuration — `araya/config.hpp`

Config is a plain `std::map<std::string,std::string>` (`plugin_config`). Typed access is
strict — "optional presence, strict values":

```cpp
static constexpr araya::config_key<int> port{"port"};

int p = araya::plugin_config_view(cfg)[port];           // throws config_error if missing/malformed
auto m = araya::plugin_config_view(cfg).try_get(port);  // nullopt only when absent
```

`config_key<T>` binds the type to the key name, so a wrong-typed access does not compile.
`try_get` returns nullopt only when the key is *absent*; a present-but-malformed value still
throws `config_error`. Specialize `araya::config_parser<T>` for your own types, or use
`araya::parse_value<T>(text)` for argv and other inputs. A `config_field` (built with
`araya::field(key, description, default, required)`) is metadata the host uses to validate
overlays and render `--print-config`.

### 4.7 Native modules — `araya/abi.hpp`, `araya/module_loader.hpp`

A native plugin is a shared object exporting `araya_plugin_entry_v1`. The boundary is plain C:
no exceptions, no STL types, no RTTI, no virtual interfaces cross it; all functions return an
error code. Every host function and module callback runs on the control strand.

`araya::module_loader::load(path)` `dlopen`s the module and returns a `plugin_descriptor`. The
`dlclose` is deferred until the last descriptor, plugin instance, and listener callback
produced from the module are gone, so a module is never unmapped while its code or vtables can
still run.

The ABI does **not** expose bail dispatch, availability checks/promotion, or the typed
config/lease facades — native modules see the core semantics plus the four event modes.

### 4.8 Optional typed facades — `araya/typed.hpp`

The erased API (`plugin_context`) is the primary surface. `typed.hpp` adds opt-in compile-time
ergonomics, all facades over the same runtime (a bad access is still rejected by the engine at
runtime; these make it a compile error too):

- `capabilities<Ks...>` / `typed_context<Caps>` — constrain `require`/`find`/`provide` to a
  declared capability set.
- `tagged_lease<T, Tag>` — make "this lease came from the committed view" a type-level fact.
- `typed_handle<State>` — mirror the fiber lifecycle in the type.
- Witness concepts: `reversible_effect` and `commutative_key`.

None of these changes runtime semantics.

### 4.9 Threading rules, in one paragraph

All composition runs on the runtime's control strand. The awaitable entry points — `mount`,
`retire`, `reconcile`, `wait_idle` — hop onto it for you. A few synchronous operations require
you to already be there: `event_bus` dispatch/registration, `plugin_context::set_available`,
and `runtime::run_on_strand`, which is the escape hatch from any thread or coroutine. For
events there is a second escape hatch: `event_bus::submit`/`submit_nowait` are safe from any
thread and hand the dispatch to the strand, so several producers can feed one bus. Component
`apply` bodies run on the strand; offload real work to your own executors and come back to the
strand to touch the context.

---

## 5. A worked sketch

A component that depends on a database, publishes a cache, and cleans up via tracked effects:

```cpp
#include "araya/plugin.hpp"
#include "araya/plugin_context.hpp"

static constexpr araya::service_key<database> db_key{"db", 1};
static constexpr araya::service_key<cache>    cache_key{"cache", 1};

struct cached_reader : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        auto db = ctx.require(db_key);                 // waits until a provider exists

        auto c = std::make_shared<cache>();
        auto prov = ctx.provide(cache_key, c);         // tracked; auto-unpublished at teardown

        // Capture the published value, not the temporary:
        ctx.on(araya::event_key<std::string, araya::dispatch_mode::serial>{"files.changed"},
               [c](std::string const& path) -> araya::task<void> {
                   c->invalidate(path);
                   co_return;
               });

        co_return;
    }
};
```

The host assembles and drives it:

```cpp
auto rt = std::make_shared<araya::runtime>(io.get_executor());

auto db    = co_await rt->mount(db_spec);
auto reader = co_await rt->mount(reader_spec);   // only activates because db is active

// Later: swap the database provider. Consumers are re-evaluated automatically.
co_await rt->retire(db);
auto db2 = co_await rt->mount(db2_spec);

co_await rt->wait_idle();   // quiescent: guaranteed by Theorem 73
```

At the end, retire everything (or just destroy the runtime) and the teardown runs in reverse
order with the ordering guarantees of §3.2.

---

## 6. Why the guarantees are trustworthy

Araya does not ask you to take the theorems on faith. The engine is held to the paper's
calculus by a three-tier verification rig (`proof/README.md`):

- **Tier 1 — conformance oracle.** An independent restatement of the paper's nine rules,
  written without including any `araya/` header, is driven with identical operation sequences
  to the real engine. Every sequence of length 3 over the full op set, every mount/retire
  sequence of length 4, and seeded 60-operation chaos sequences must produce identical
  observables (fiber states, error presence, bindings, and exact apply/cleanup order). This is
  the empirical link between the model and `src/runtime.cpp`.
- **Tier 2 — TLA+ refinement.** `proof/tla/` contains the paper's calculus (`PaperRules.tla`),
  the concrete engine machine (`ArayaMachine.tla`), and the refinement mapping
  (`Refine.tla`). TLC exhaustively checks that every concrete step is a paper rule or a
  stutter, and checks quiescence as a liveness property, over a three-slot universe. It runs
  as part of `ctest`.
- **Tier 3 — Coq.** A parked, partial generalization of the model check to all instances
  (`proof/coq/`); not wired into the build.

The maintenance rule is the interesting part for contributors: **a change to observable
semantics extends the TLA+ models and the conformance oracle in the same commit**, before the
engine code changes. A change that only touches internal data structures (a *stutter* under
the abstraction) needs no proof work — the rig re-runs green. This is how the engine can be
optimized without re-proving the theory each time, and it is why the guarantee is credible
rather than aspirational.

Two honest limitations are documented there:

1. The models check a **bounded universe** (a few slots and components), not every instance.
2. Real multi-strand concurrency would add interleavings the current model does not express;
   such a change must extend the model's scheduling story first.

---

## 7. Paper-to-code map

If you do read the paper, this table is the bridge. The theory terms come from *A Programming
Paradigm for Spatiotemporal Composability*; the code is Araya.

| Paper concept | Araya |
|---|---|
| Context Γ | `araya::context`, reached via `plugin_context` |
| Component `(d, p, e)` | `plugin_descriptor` + `plugin` |
| Fiber `⟨d,p,e,π,σ,τ,θ⟩` | a `fiber_record` in `runtime`, named by `fiber_handle` |
| Lifecycle θ (`Inactive`/`Reloading`/`Active`/`Unloading`) | `fiber_state` |
| Accumulator `g` | the fiber's `effect_stack` (run LIFO) |
| Committed view ω | the activation's `committed_view` / `fiber_info::committed` |
| O-Insert / O-Retire | `runtime::mount` / `runtime::retire` |
| Instantiation (Definition 52) | `plugin_context::mount` |
| L-Begin / L-Iter / L-Finish | `apply`'s coroutine steps |
| L-Leave / L-Divert / L-Unload | the ordered-withdrawal path in `runtime.cpp` |
| Guard `reliedₙ` (Definition 54) | the consumer index awaited before provider cleanup |
| Target view (Definition 53) | recomputed from committed views + provider state |
| Confluence (Theorem 80) | `runtime::reconcile`'s soundness |
| Preservation (Theorem 64) | `runtime::validate_invariants` |

The short version: Araya gives you fine-grained, runtime composition with cleanup you do not
have to write, dependency wiring the runtime performs for you, and mathematical guarantees
that your reloads converge and that removal is exact — all checked, not just claimed.
