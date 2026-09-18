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
- `tla/MC.tla`/`MC.cfg`: three slots and the five-component pool shared
  with the C++ universe (the C++ tests drive two slots; the third widens
  the interleaving net). TLC checks (a) the refinement, (b) quiescence
  as a liveness property (Theorem 73) under weak fairness on the
  internal actions: 441,818 distinct states, no violation, ~16 s.

The model check is wired into ctest unconditionally: it runs whenever
Java and tla2tools.jar are found (on `TLA2TOOLS` or in the usual
locations) and is skipped with an INFO message otherwise. Run it via
`ctest -R tlc_refinement` or `tla/run-tlc.sh`. Coq (tier 3) is never
wired into the build.

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

## What the rig does not cover (accepted limitations)

1. **The model is not the code.** TLC proves properties of
   `ArayaMachine.tla`; the tier-1 conformance suite is the empirical link
   to `runtime.cpp` (exhaustive bounded sequences plus seeded chaos). If
   the model drifts from the engine, conformance catches it — which is
   why the maintenance rule requires model + code changes in the same
   commit.
2. **Single-strand scheduling.** The model encodes the engine's one-strand
   serialization. Real concurrency across strands would add
   interleavings the model cannot express; such a change must extend the
   scheduling story in the model first.
3. **Bounded universe.** TLC checks three slots and five components
   exhaustively — not every instance. Deeper cascades than three slots
   fall outside the model check (the Coq tier was the generalization;
   it is parked). The C++ chaos suites remain the wider net.
4. **The paper's metatheory is assumed.** The refinement transfers
   Section 4.3 (Theorem 64/68/70/73/80) as proven in the paper; the
   model check validates the *refinement*, not the paper's theorems.
5. **Values are abstracted.** The model reduces the data plane to one key
   (`example.db`) and abstracts the components' effect functions; what
   `apply()` computes is outside the calculus, by the paper's own
   confinement discipline (Definitions 55–56).

## Tier 3 — Coq (parked: model + invariants proven, refinement deferred)

A Coq development under `coq/` (commit `ccf9e8a`) generalizes the TLA+
check from its two-slot universe to *all* instances:

- `coq/Machine.v` — the concrete machine as a faithful Coq transcription
  of `tla/ArayaMachine.tla` (one inductive step per action, each citing
  its paper rule and `src/runtime.cpp` function); components are a type
  parameter, slots an unbounded nat.
- `coq/PaperCalculus.v` — the paper's calculus at the abstraction level:
  the nine rules, the Failure extension, Definition 53's target
  (bottom ≠ empty), Definition 54's relied, eq. (49) quiet, Definition 63
  well-formedness.
- `coq/ConcreteInvariants.v` — the 21-clause `Good` invariant (the
  optimization-guard contracts: well-formedness, the guard counter
  implementing reliedₙ, the consumers index, worklist bookkeeping,
  retirement stickiness, the failure outcome, the finite-name universe)
  with preservation proofs for all 13 actions, `init_good`,
  `good_step`, `reach_good`, and per-clause projections prepared for the
  refinement proof.

Status: `Machine.v` and `PaperCalculus.v` check in under a second.
`ConcreteInvariants.v`'s proofs are written in full, but the clause
tactic (`eauto 8`) makes the check impractically slow (~25 min) — the
file is committed as-is and parked. Not yet written: `Refinement.v`
(the α simulation that transfers the paper's Section 4.3 metatheory to
the engine for all instances) and `Progress.v` (Theorem 73's
no-deadlock). The debugging that remains is tactical, not mathematical:
a shape-dispatched solver (eauto 4 + explicit clause matching) is the
known fix.

Why parked: for the intended use — guarding engine optimizations — the
tier-2 model check is exhaustive on the shared universe and finishes in
~1.5 s, and tiers 1+2 already hold the engine to the paper. Coq's value
(unbounded generalization, machine-checked transfer of the paper's
theorems) is real but not needed at this stage; it is a fun exercise for
later.
