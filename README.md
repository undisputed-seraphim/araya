# Araya

A C++23 runtime for applications assembled from components that may appear, disappear, or be
replaced while the process remains live.

Araya adapts the ideas of _A Programming Paradigm for Spatiotemporal Composability_
(Y. Shi, W. Zhang, T. Cui — arXiv:2608.25512) and its reference implementation Cordis,
without adopting their TypeScript-specific API or implementation choices.

## The model, briefly

The paper identifies two dimensions of dynamic composability. **Temporal composability**
demands that removing a component withdraws everything it installed in the shared environment;
**spatial composability** demands that components declare their dependencies and activate only
when they resolve. Araya implements both in their global forms:

- **Revertible effects.** Every managed operation (providing a service, registering a listener,
  acquiring a resource) records its cleanup the moment it succeeds, and an activation's cleanups
  run in reverse order on teardown. A component's `apply()` coroutine is the paper's "effect
  iterator": each `co_await` is a step boundary, so an activation interrupted mid-load stops at a
  boundary, runs the cleanup accumulated so far, and is never published.
- **Reactive coeffects.** A component declares `inject` (what it requires) and `provide` (what it
  may publish). Every binding change is classified against those declarations: unaffected, or
  deactivated, or deactivated-and-reactivated. Resolution records the *provider identity*, not the
  value, so replacing a provider re-evaluates its committed consumers.
- **Ordered withdrawal.** When a provider leaves, it stops accepting new consumers first;
  committed consumers tear down while still holding their provider's bindings; the provider's
  own cleanup runs only after every consumer has reached `inactive`. This mirrors the paper's
  `L-Leave`/`L-Unload` guard.
- **Serialized topology.** All composition changes run on a control strand above an Asio
  executor; component work is asynchronous and cooperatively cancellable through
  `std::stop_token`. Transition completion re-checks the committed view ("inertia" in the
  paper's terms).
- **Reconciliation.** The host supplies a desired tree of component specs; the runtime diffs it
  against live fibers: retain, mount, retire, replace, or apply a supported config update in place.
  Native modules are loaded with `dlopen` and unloaded only once no fiber, listener, or
  callback still reaches them.

As in the paper, the guarantees hold within the managed boundary: Araya cannot undo emitted
network traffic, reclaim detached threads, or sandbox hostile native code.

## Building

- C++23 compiler (GCC 14+ or Clang 19+)
- Boost ≥ 1.83 (header-only Asio)
- CMake ≥ 3.24, Ninja recommended
- Catch2 v3 (optional, for tests)

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
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

## Public API (overview)

```cpp
namespace araya {

// --- fibers and tasks ------------------------------------------------------

template <class T = void>
using task = boost::asio::awaitable<T, boost::asio::any_io_executor>;

struct fiber_handle {
    fiber_id id() const;
    fiber_state state() const;         // inactive / loading / active / unloading
    void cancel() const;               // cooperative: request_stop only
};

fiber_handle spawn(boost::asio::any_io_executor, task<T>);   // ad-hoc fiber
boost::asio::awaitable<std::stop_token> this_stop_token();
boost::asio::awaitable<bool> stop_requested();

// --- services and scopes ----------------------------------------------------

template <class T> struct service_key { service_id id; };    // name + version + C++ type
template <class T> class service_lease;                      // committed-view access

class context {                                              // scoped binding tree
    void bind(service_id, binding);
    void unbind(service_id);
    binding const* lookup(service_id) const;
    service_metadata metadata_for(service_id) const;         // per-key policy metadata
};

// --- the runtime (plugin manager) -------------------------------------------

class runtime {
public:
    explicit runtime(boost::asio::any_io_executor);
    plugin_context root_context();
    boost::asio::awaitable<fiber_handle> mount(component_spec);
    boost::asio::awaitable<void> retire(fiber_handle);
    boost::asio::awaitable<void> reconcile(std::vector<desired_component>);
    boost::asio::awaitable<void> wait_idle();
    boost::asio::awaitable<void> run_on_strand(std::move_only_function<void()>);
    std::shared_ptr<event_bus> bus() const;
};

// --- plugins ------------------------------------------------------------------

class plugin {
    virtual boost::asio::awaitable<void> apply(plugin_context&) = 0;
    virtual bool reconfigure(plugin_config const&) { return false; }  // in-place updates
};

struct plugin_descriptor {
    std::string_view name;
    std::span<dependency_spec const> inject;   // {key, required}
    std::span<provision_spec const> provide;
    std::function<std::unique_ptr<plugin>(plugin_config const&)> create;
};

class plugin_context {
    template <class T> service_lease<T> require(service_key<T> const&);
    template <class T> std::optional<service_lease<T>> find(service_key<T> const&);
    template <class T> registration provide(service_key<T> const&, std::shared_ptr<T>);
    registration effect(std::move_only_function<cleanup_action()>);
    template <event_key K, class Fn> registration on(K const&, Fn&&);
    std::stop_token stop_token() const;
};

// --- events -------------------------------------------------------------------

enum class dispatch_mode { emit, parallel, serial, waterfall };
template <class Message, dispatch_mode Mode> struct event_key { service_id id; };

class event_bus {   // typed, strand-bound; listeners are owned effects
    void set_diagnostic_sink(std::move_only_function<void(std::exception_ptr)>);
    // dispatch(key, msg): emit = fire-and-forget; parallel/serial = awaitable;
    // waterfall = chain with short-circuit/transform via waterfall_continuation
};

// --- native modules ------------------------------------------------------------

class module_loader {
    std::shared_ptr<plugin_descriptor> load(std::string const& so_path);  // dlopen;
    // dlclose deferred until the last descriptor / instance / listener is gone
};

// each .so exports:
//   extern "C" const araya_plugin_descriptor_v1*
//   araya_plugin_entry_v1(const araya_host_api_v1*);

}  // namespace araya
```

## Status

**Alpha.** The public API and internal design are subject to change. The core semantics
(effects, reactivity, lifecycle ordering, reconciliation, the native module ABI) are implemented
and covered by 70 tests, but there is no stability guarantee yet.

## License

Copyright © 2026 Tan Li Boon. Licensed under the [Apache License, Version 2.0](LICENSE).
