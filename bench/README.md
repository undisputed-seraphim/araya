# araya_bench

The profiling bench and example host application for the Araya engine.

## Who plays what role

- **The bench `main()` is the host — the paper's orchestrator.** Araya is a
  static library and its plugins are libraries, not entrypoints; every
  deployment needs a `main()` that owns the `io_context`, constructs the
  runtime, and issues the first mounts. The host is deliberately not a
  plugin: the calculus has an irreducible outside that boots the engine
  and drives the strand.
- **Everything with a fiber identity is a plugin.** The `steady` workload
  demonstrates the entrypoint-as-plugin pattern: `main()` mounts exactly
  one app component, whose `apply()` assembles its own subgraph through
  `ctx.mount(...)` — a tracked effect, so unloading the app retires its
  children (Theorem 73).
- The workloads emit the same action vocabulary the TLA+ model checks
  (`proof/tla/MC.tla`): mount, retire, replace, reconcile. Bench traffic
  and model traffic are comparable, which is what keeps the optimization
  loop honest: after any engine change, re-run `ctest -R tlc_refinement`
  plus the conformance suite — commit only when green.

## Build

Profile with optimizations on and symbols in:

```
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-bench --target araya_bench
```

Two optional knobs:

- `-DARAYA_BENCH_FP=ON` — `-fno-omit-frame-pointer`, for perf's
  frame-pointer call graphs.
- `-DARAYA_BENCH_GPROF=ON` — links gperftools into the binary so
  `CPUPROFILE`/`HEAPPROFILE` work without `LD_PRELOAD`.

## Workloads

```
./build-bench/bench/araya_bench --workload all --iterations 1000
```

- `mount` — one provider, repeated mount+retire of required consumers
  (activation, guard accounting, unload churn)
- `cascade` — a dependency chain built deepest-first, then the terminal
  provider triggers the apply cascade; retiring it tears the chain down
- `replace` — provider replacement storms under the single-source
  discipline (retire old, insert new, notify fan-out, guard re-dispatch)
- `reconcile` — random desired-tree diffs through the declarative host
  interface (`runtime::reconcile`), fixed seed
- `steady` — one app component + N ticker children driving 10 ms timer
  intervals with debug logging (the entrypoint-as-plugin pattern)

Options: `--iterations N`, `--components N` (steady tickers),
`--depth N` (cascade), `--steady-ms T`, `--log-level error|warn|info|debug`.
Each run ends with `wait_idle()` + `validate_invariants_async()`; a
violation aborts instead of skewing the numbers. Output is one
machine-readable line per workload:

```
workload=mount     ops=20000 wall_ms=61.905 per_op_ns=3095.3 fibers=2
```

## Profiling runbook

Capture a baseline before optimizing, and after every change compare
against it — same binary, same flags, same iteration count.

### perf (CPU)

```
perf record -g --call-graph dwarf -- ./build-bench/bench/araya_bench --workload mount --iterations 50000 --log-level error
perf report
```

Quick counters without the record/report cycle:

```
perf stat -e cycles,instructions,cache-misses,branch-misses -- \
    ./build-bench/bench/araya_bench --workload all --iterations 5000 --log-level error
```

### gperftools (CPU)

Build with `-DARAYA_BENCH_GPROF=ON`, then:

```
CPUPROFILE=/tmp/cpu.prof CPUPROFILE_FREQUENCY=1000 \
    ./build-gprof/bench/araya_bench --workload mount --iterations 100000 --log-level error
google-pprof --text ./build-gprof/bench/araya_bench /tmp/cpu.prof
```

Note: link the combined `tcmalloc_and_profiler`, not `-lprofiler` and
`-ltcmalloc` side by side — the CPU profiler never starts otherwise (the
CMake knob already does this).

### gperftools (heap)

```
HEAPPROFILE=/tmp/heap \
    ./build-gprof/bench/araya_bench --workload replace --iterations 20000 --log-level error
google-pprof --text --inuse_space ./build-gprof/bench/araya_bench /tmp/heap.0001.heap
```

Expect the workload to run much slower: every allocation is recorded.

### heaptrack / valgrind

```
heaptrack ./build-bench/bench/araya_bench --workload replace --iterations 20000 --log-level error
heaptrack_print heaptrack.araya_bench.*.zst
valgrind --tool=massif ./build-bench/bench/araya_bench --workload replace --iterations 5000 --log-level error
```

## Optimization loop (the contract)

1. Classify the change: stutter-space (data structures, allocation,
   indexing) or observable (lifecycle states, guard, committed views,
   binding map, dispatch order).
2. Stutter-space: no model edits — the bench and the TLA+ check re-run
   green and the numbers are the point.
3. Observable: extend `proof/tla/` and the conformance oracle first, run
   `ctest -R tlc_refinement` **before** touching the C++, then implement.
4. Always: full `ctest` (conformance suite) + `tlc_refinement` green
   before committing; a TLC counterexample names the action, which maps
   1:1 to a `src/runtime.cpp` function.
