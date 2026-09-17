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

## Tier 2 — TLA+ refinement checking (next)

Planned layout: `tla/PaperRules.tla` (the nine rules over interleavings,
with the abstract component table), `tla/ArayaMachine.tla` (the concrete
fine-grained machine), `tla/Refine.tla` (the α mapping and the
SPECIFICATION-Impl / PROPERTY-Abs-α refinement check), `tla/MC.cfg`, and
`tla/run-tlc.sh`. Because the engine serializes everything through one
strand, the only true interleaving is operation order — which is exactly
what TLC enumerates exhaustively, including the bounded state space the
single-threaded tests cannot see.

Model checking is opt-in (`ARAYA_ENABLE_PROOF`, needs Java +
tla2tools.jar on PATH or in `TLA2TOOLS`).

## Tier 3 — deferred

A Coq/Lean simulation proof of the α mapping would turn the bridge into a
theorem (paper theorems transfer to a model of the engine). Recorded as
"fun later": it needs a Coq-capable contributor, and tiers 1+2 are the
sustainable rig for an evolving core.
