# Araya

A C++23 runtime for applications assembled from components that may appear, disappear, or be
replaced while the process remains live.

Araya adapts the component calculus of _A Programming Paradigm for Spatiotemporal
Composability_ (Y. Shi, W. Zhang, T. Cui — arXiv:2608.25512): components install reversible
effects into a shared environment, activate only when their declared dependencies resolve, and
are withdrawn in a fixed order that lets dependents tear down safely. The observable semantics
are continuously checked against the paper's transition rules by a TLA+ refinement model
checker, so any engine feature that extends the calculus is added model-first and verified
before it lands in the engine.

## The lifecycles

Everything a component does — and everything the runtime does to it — happens inside one of
the lifecycles below. They are the interface contract; the model checker holds the engine to
them.

### Fiber lifecycle

A **fiber** is one mounted component instance. It moves through four states:

```
inactive ──▶ loading ──▶ active ──▶ unloading ──▶ inactive
```

- `plugin::apply(plugin_context&)` is the *loading* phase, written as a coroutine. Each
  `co_await` in `apply` is a step boundary: if the activation is cancelled or throws
  mid-load, the cleanup accumulated so far runs in reverse order and the fiber is never
  published.
- A fiber becomes `active` once `apply` returns; at that point its provisions are visible to
  other components and its listeners may be invoked.
- **Ordered withdrawal.** When a provider leaves, the runtime first stops handing it new
  consumers; already-committed consumers tear down *while still holding their provider's
  bindings*; the provider's own cleanup runs only after every consumer has reached
  `inactive`. A component being torn down can therefore still read the dependencies it was
  activated against.
- `apply` is cooperative: watch `plugin_context::stop_token()` (or `this_stop_token()` /
  `stop_requested()` in ad-hoc fibers) and return promptly when it fires. `fiber_handle::
  cancel()` is a stop *request*, never a preemption.

### Effect lifecycle

Every managed operation (providing a service, registering a listener, acquiring a resource)
records its cleanup the moment it succeeds:

```cpp
araya::registration r = ctx.effect([&] {
    install_something();
    return araya::cleanup_action{[&] { remove_something(); }};
});
```

- The `registration` owns the cleanup. Unless `release()`d early, cleanups run in reverse
  order when the fiber's activation is torn down — the paper's accumulator discipline.
- `provide` and `on` return the same kind of `registration`; releasing one un-publishes that
  single provision or listener without touching the rest.

### Binding and availability lifecycle

A component declares its dependencies and provisions; the runtime enforces them:

```cpp
static constexpr araya::service_key<database> db{"db", 1};

araya::task<void> apply(araya::plugin_context& ctx) {
    auto db_lease = ctx.require(db);            // throws resolution_error if absent
    auto maybe    = ctx.find(db);               // std::nullopt if absent
    ctx.provide(db, my_db);                     // throws if 'db' was not declared
}
```

- **Declare, then use.** `require`/`find` of a key you did not declare is a logic error; so is
  providing one you did not declare. Declarations are immutable for the fiber's lifetime.
- Resolution records the *provider identity*, not the value: replacing a provider re-evaluates
  its committed consumers, so consumers never keep a stale binding silently.
- **Availability.** `provide(key, value, check)` evaluates `check` *once* at provide-time; a
  failing or throwing check publishes the binding as unavailable. An unavailable binding
  behaves as absent: required consumers park, optional ones proceed with the empty view.
  The provider later promotes it — and only promotes it:

```cpp
ctx.set_available(db, true);   // the only transition; idempotent; must run on the strand
```

  There is no `set_available(db, false)`: it throws. In the paper's lifecycle there is no
  edge back from active to loading, so deactivation is not expressible — to withdraw a
  service, unload its provider (`runtime::retire`), which runs the ordered withdrawal above.

### Service-lease lifecycle

`require`/`find` return a `service_lease<T>`: a `shared_ptr` to the value plus the providing
fiber's id and merged metadata.

- Retaining a lease keeps the *value* alive past `apply` — the provider cannot be unloaded
  out from under the object itself.
- The *binding* is not a lease: after teardown the same key may be re-provided by a different
  fiber. Hold a lease for the object; re-`require` for the current resolution.

### The plugin context lifecycle

`plugin_context` is a **view** into one activation, valid only while that activation runs:
inside `apply`, and inside the listeners and cleanups that activation registered. Never store
it — by the time any stored copy could be used, the activation it describes may be unloading.

- `plugin_context::root()` walks to the runtime's root scope.
- `plugin_context::mount(spec)` instantiates a child component; the instantiation is an
  ordinary tracked effect, so unloading the parent cascades to its children.

### Event-listener lifecycle

```cpp
static constexpr araya::event_key<std::string, araya::dispatch_mode::serial>
    files{"files.changed"};

araya::registration r = ctx.on(files, [](std::string const& path) -> araya::task<void> {
    co_await rescan(path);
});
```

- The listener belongs to the registering fiber: it is removed automatically at teardown, or
  earlier via the returned `registration`.
- `listener_options` adjust delivery: `prepend` (ahead of existing listeners), `once` (runs at
  most once), `global` (delivers regardless of dispatching scope), and `scope` (delivers only
  to dispatches carrying a scope with the same realm label).
- Dispatch modes fix the listener signature and the flow: `emit` fire-and-forget, `parallel`
  all listeners awaited together, `serial` in registration order, `waterfall` a chained
  pipeline where each listener may transform the message, `bail` a chain that short-circuits
  on the first listener returning `true`.
- Registration and dispatch must run on the control strand (see below). Listener errors go
  to the bus's diagnostic sink (`set_diagnostic_sink`); `parallel` dispatch additionally
  rethrows the first listener failure at the dispatcher.

### Config lifecycle

Components receive a `plugin_config` (string map) through their factory. Typed access is
strict — "optional presence, strict values":

```cpp
static constexpr araya::config_key<int> port{"port"};

int p = araya::plugin_config_view(cfg)[port];      // throws config_error: missing/malformed
auto m = araya::plugin_config_view(cfg).try_get(port); // nullopt only when absent
```

`config_key<T>` binds the C++ type to the key name, so a wrong-typed access does not compile;
`araya::parse_value<T>(text)` is the free-function form for argv and other inputs; specialize
`araya::config_parser<T>` for your own types.

- `plugin::reconfigure(config)` is the in-place update hook. Returning `false` means "no
  in-place update": the runtime deactivates and reactivates the fiber with the new config —
  a full reload through the fiber lifecycle.

### Native-module lifecycle

A native plugin is a shared object exporting `araya_plugin_entry_v1` (see `araya/abi.hpp`:
a plain-C ABI, strand-bounded, error codes only). `module_loader::load` dlopens it and hands
back a `plugin_descriptor`; the `dlclose` is deferred until the last descriptor, plugin
instance, and listener callback produced from the module are gone, so a module is never
unmapped while any of its code or vtables can still run.

### Threading in one paragraph

All composition runs on the runtime's **control strand** (an Asio strand). The awaitable
entry points — `mount`, `retire`, `reconcile`, `wait_idle` — hop onto it for you. A few
synchronous operations require you to be there already: `event_bus` dispatch/registration,
`plugin_context::set_available`, and `runtime::run_on_strand`, which is the escape hatch
from any thread or coroutine. Component `apply` bodies run on the strand; offload real work
to your own executors and return to the strand to touch the context.

## Building

- C++23 compiler (GCC 14+ or Clang 19+)
- Boost ≥ 1.83 (header-only Asio)
- CMake ≥ 3.24, Ninja recommended
- Catch2 v3 (optional, for tests)
- Java 21 + the TLA+ tools jar (optional, for the TLC model check; the build skips it with a
  notice when absent)

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Memory checking

The whole suite is clean under ASan + UBSan + LSan:

```sh
cmake -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -Wno-error=stringop-overflow" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/asan
ctest --test-dir build/asan
```

`-Wno-error=stringop-overflow` silences a GCC 14 false positive in the sanitizer-instrumented
session plugin builds; it is not needed for normal builds.

ThreadSanitizer, same idea (GCC; `-Wno-tsan` silences GCC's "fences not supported with
tsan" diagnostic, and Clang cannot link the GCC-LTO `libboost_json` this machine ships):

```sh
cmake -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -Wno-tsan -Wno-error=tsan" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/tsan
ctest --test-dir build/tsan
```

Araya is a static library and provides a CMake package. To install:

```sh
cmake --install build --prefix /path/to/prefix
```

and consume it from another project:

```cmake
find_package(araya 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE araya::araya)
```

The `bench/araya_bench` target is a complete example host exercising every lifecycle above
and doubles as the profiling benchmark.

## The applications

The unified `araya` executable (`apps/araya`, everything first-party statically linked)
ships two surfaces, dispatched on the first argument: the FTXUI terminal UI for
interactive use, and a headless script runner (the retired interactive console's
replacement) that the ctests drive.

### Script mode

`araya run <script>` mounts the first-party plugins plus the demo components (the
console, a beacon/watcher availability pair, and failure-injection bombs) and replays
a command script line by line. Every command goes through the public engine surface,
so every outage a command reports is the availability machinery working, not a
special path.

```sh
cmake --build build --target araya_app
./build/apps/araya/araya run apps/araya/demo/demo.txt   # scripted tour
```

Things to watch in the scripted tour:

- `fail timer` swaps in a crashing provider: the bomb fails, the console (its dependent)
  unloads, and `timer in ...` reports the outage. `fail clear` restores everything.
- `avail wait` retracts the beacon with its readiness flag cleared: the watcher parks
  ("unresolved service"). `avail ready` promotes the beacon and the watcher activates -
  the promotion-only gate, no demotion anywhere.
- `unload console` retires the console plugin (watch its heartbeat die in the logs);
  `load console` brings it back. Sessions survive because the session plugin is untouched.
- `session new/append/show` drives the event log and prints the folded message surface;
  `session replace <s> <e> <text>` splices a span of history into one message (the
  compaction mechanism), and `session fork <parent> [child]` opens a child with an
  inherited log prefix. The live feed shows the `session/created`, `session/event`,
  and `session/disposed` firehose.
- `session save` flushes through the `session/flush` durability barrier (the persistence
  plugin drains and fsyncs), and `session load`/`load-all` restore sessions from disk
  through the same `prepare`/`enter` path the engine models. Sessions land as readable
  JSONL under `./araya-sessions/` - `cat` one while you work. The store also drives
  typed per-session projections (see `araya/session/projection.hpp`) on the same event
  stream, so derived state survives restore by replay.

The demo and the save/restart/load cycle are registered as the `script_demo` and
`restart_cycle` ctests, so they run with the rest of the suite.

### The TUI

`araya tui` (FTXUI vendored in `thirdparty/`) is the interactive terminal UI and the
console's intended successor: the same desired tree boots on a background engine thread
while the UI renders live component states, a session-event log pane, and a command
input - the UI only ever reads immutable snapshots, so the engine's strand discipline
stays entirely on the engine side. Logs go to `araya-tui.log` (pre-created file-sink
loggers the logger service adopts by name), keeping quill's output off the canvas.
Requires a terminal; Ctrl+D quits (the `quit` command works too), and Ctrl+C is
swallowed per TUI convention. Component state uses unicode glyphs (`●` active, `◐`
transitioning, `✗` failed, `○` retired); `ARAYA_TUI_ASCII=1` switches to the ASCII
tier (`* ~ x o`) for terminals whose fonts misrender them.

## License

Copyright © 2026 Tan Li Boon. Licensed under the [Apache License, Version 2.0](LICENSE).
