# Proof — the refinement bridge to the paper

Araya's correctness argument against _A Programming Paradigm for
Spatiotemporal Composability_ (arXiv:2608.25512): assuming the paper's
metatheory (Preservation, Theorem 68/70/73/80), we show the engine is a
refinement of the paper's rule system, and that the code matches the model
on exhaustive bounded tests.

## The abstraction contract

The bridge's load-bearing decision is what counts as observable state
(α-visible) versus internal machinery (a stutter under α):

- **Observable**: fiber lifecycle states, the `remaining` guard,
  committed-view contents, the binding map (key → provider), and the
  lifecycle dispatch order (apply/cleanup as seen by plugins).
- **Stutters**: strand serialization, notify-index representation, the
  `-1` retirement flag, `on_apply_completed`/`providers_still_active`
  internals — and any future data-structure optimization.

**Maintenance rule**: a change touching only stutters requires zero proof
work (CI re-checks green). A change to observable semantics extends
`oracle/oracle.hpp` and the TLA+ models in the same commit. This is the
whole ongoing-cost story: the rig re-runs, it is never re-built.

**Operation semantics**: an operation (mount / retire / reconcile) is
defined to run to quiescence — the engine driver awaits `wait_idle()`
after every op. The engine permits observing mid-transition states (the
paper's Asynchrony/inertia regime, pp. 55–56); the conformance contract
simply pins the quiescent interleaving, which is the observable one.

## Tier 1 — the paper oracle (implemented)

- `oracle/oracle.hpp`: an independent restatement of the nine rules plus
  the extensions Araya adopts — the inertia regime, the Failure extension
  (a raise withholds re-entry until revision), and self-provision
  exclusion. Deliberately free of any `araya/` include: it is written from
  the paper and the engine's documented semantics, so it cannot inherit
  implementation bugs by construction.
- `conformance/conformance_test.cpp`: drives the real engine and the
  oracle with identical operation sequences and requires identical
  observables (fiber states, error presence, the `example.db` binding,
  and the exact apply/cleanup order). Coverage:
  - every sequence of length 3 over the full op set (mount / retire /
    reconcile with config changes, six components, two slots),
  - every mount/retire sequence of length 4,
  - 40 seeded 60-op chaos sequences.

## Findings so far (the rig already pays rent)

1. **Self-provider livelock (engine)**. Two self-providing components of
   one key (the paper's single-source violation, which Araya diagnoses but
   deliberately permits) commit each other's bindings and invalidate each
   other forever: the engine never quiesces. The conformance universe
   encodes the single-source discipline by construction (S is confined to
   one slot). Worth revisiting: the diagnostic could be promoted to a
   quiescence guard.
2. **Reconcile is whole-state, not per-slot.** `runtime::reconcile`
   retires every declared path absent from the desired set — per-slot
   mental models of reconcile are wrong; the oracle models the full
   declared state.
3. **Cascade must iterate a copy.** While chasing (2), the oracle's
   cascade loop hit the same committed-set iterator invalidation the
   engine once fixed — the two implementations agreeing on the *bug shape*
   is weak evidence the restatement is faithful.
4. **Shutdown strand teardown (open).** The conformance binary (which
   constructs ~37k runtimes) hangs in the last `strand_impl` destructor at
   process exit, blocked on the strand mutex. Test results are unaffected
   (all assertions pass before teardown); this is an Araya teardown
   wrinkle at the Asio layer to investigate.

## Tier 2 — TLA+ refinement checking (implemented)

- `tla/PaperRules.tla`: the paper's calculus (Section 4.2, pp. 34-37) at
  the abstract level — all nine rules (O-Insert/O-Retire/O-Remove,
  L-Begin/L-Iter/L-Finish, L-Divert/L-Leave/L-Unload) plus the
  Asynchrony (p. 55) and Failure (p. 56) extensions, Definition 53's
  target view (with bottom ≠ empty view), Definition 54's guard, and
  eq. (49) quiescence. Every rule cites its page.
- `tla/ArayaMachine.tla`: the concrete fine-grained machine, one action
  per rule step, each citing its `src/runtime.cpp` function. The
  stutter-vs-observable split is the README's abstraction contract.
- `tla/Refine.tla`: the α mapping and the refinement — every concrete
  step is a paper rule or a stutter.
- `tla/MC.tla`/`MC.cfg`: two slots and the five-component pool shared
  with the C++ universe. TLC checks (a) the refinement, (b) quiescence
  as a liveness property (Theorem 73) under weak fairness on the
  internal actions: 4,383 distinct states, no violation.

Model checking is opt-in (`ARAYA_ENABLE_PROOF`, needs Java +
tla2tools.jar on PATH or in `TLA2TOOLS`); run via `ctest -R
tlc_refinement` or `tla/run-tlc.sh`.

### What the model check teaches (and what it ruled out)

The first model drafts failed refinement, and every failure was a
*fidelity* bug in the model, not in the paper or the engine — but each
forced out a precise statement of why the engine is safe:

1. A retired fiber can never publish: the engine's retire flips a loading
   fiber to unloading in the same handler (the paper's L-Finish reads
   the same way through the τ premise of target).
2. The engine's one-directional stale check (`providers_still_active`)
   is sufficient because its single-strand scheduling never lets an
   apply land after the binding map changed under it; the paper's
   two-directional target re-check (pp. 55-56) covers the unreachable
   interleavings.
3. The `remaining` counter is exactly Definition 54's guard: it counts
   only *installed* consumers, and a loading consumer is not yet relied
   — the engine keeps Theorem 70's read-through-deactivation via the
   committed-view snapshot, and the consumer's completion-time re-check
   is the paper's L-Divert.
4. A diverted fiber completes through its cancelled apply, never through
   Finish (runtime's `finish_unload` runs for active-origin unloads
   alone).

## Tier 3 — deferred

A Coq/Lean simulation proof of the α mapping would turn the bridge into a
theorem (paper theorems transfer to a model of the engine). Recorded as
"fun later": it needs a Coq-capable contributor, and tiers 1+2 are the
sustainable rig for an evolving core.
