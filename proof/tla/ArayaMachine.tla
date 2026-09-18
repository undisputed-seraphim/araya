----------------------------- MODULE ArayaMachine ----------------------------
(* The concrete machine: Araya's runtime at the same granularity as the
   paper's rules, so the refinement in Refine.tla is step-by-step. Each
   action cites its paper rule and its src/runtime.cpp function. The
   engine serializes all transitions through one control strand, so the
   only true interleaving is at operation granularity; this model splits
   the transitions into the rule steps for the correspondence, and the
   C++ oracle (../oracle/oracle.hpp) pins the exact engine order.

   Concrete state (the abstraction contract, proof/README.md):
     alive[n]      - fiber record exists (fibers_ map)
     comp[n]       - component, fixed at mount (Lemma 59(5))
     state[n]      - in / ld (loading) / ac (active) / ul (unloading)
     committed[n]  - the committed view (omega_n): provider of db, 0 none
     err[n]        - a resolution/cancellation error is recorded (stutter
                     under alpha; only afailed is the paper's outcome)
     afailed[n]    - apply raised: the Failure outcome (p. 56)
     reactivate[n] - -1 sticky retirement (tau, Lemma 59(5) monotone),
                     0 none, 1 re-evaluate after unload (the mutual
                     chaining of reload and unload, p. 56)
     remaining[n]  - the guard counter: non-inactive published consumers
                     (Definition 54 relied_n, implemented as a counter;
                     Theorem 73 shows it always releases)
     consumers[n]  - fibers that published with n in their committed view
     binding       - the db binding under the one shared realm
     bstate        - the binding's provider state (loading/active/retiring);
                     "ld" also encodes an unavailable binding (the
                     availability extension: the provider landed but has
                     not yet published from the dependents' point of
                     view - runtime::binding.check, evaluated at
                     provide-time, promoted through AvailabilityFlip /
                     runtime::signal_availability). Promotion only: the
                     paper's lifecycle DAG has no active -> loading edge,
                     so deactivation is retirement, not a flip
     wl            - the pending-apply queue, FIFO in spawn order *)

EXTENDS Integers, FiniteSets, Sequences

CONSTANTS Slot, Comp, Provides, ReqDb, FAILS, AvailInit

MaxOf(a, b) == IF a >= b THEN a ELSE b

VARIABLES alive, comp, state, committed, err, afailed, reactivate,
          remaining, consumers, binding, bstate, wl

vars == <<alive, comp, state, committed, err, afailed, reactivate,
          remaining, consumers, binding, bstate, wl>>

TypeOK ==
  /\ alive \in [Slot -> BOOLEAN]
  /\ comp \in [Slot -> Comp]
  /\ state \in [Slot -> {"in", "ld", "ac", "ul"}]
  /\ committed \in [Slot -> (Slot \cup {0})]
  /\ err \in [Slot -> BOOLEAN]
  /\ afailed \in [Slot -> BOOLEAN]
  /\ reactivate \in [Slot -> {-1, 0, 1}]
  /\ remaining \in [Slot -> Nat]
  /\ consumers \in [Slot -> SUBSET Slot]
  /\ binding \in (Slot \cup {0})
  /\ bstate \in {"none", "ld", "ac", "rt"}
  /\ wl \in Seq(Slot)

Init ==
  /\ alive = [n \in Slot |-> FALSE]
  /\ comp \in [Slot -> Comp]
  /\ state = [n \in Slot |-> "in"]
  /\ committed = [n \in Slot |-> 0]
  /\ err = [n \in Slot |-> FALSE]
  /\ afailed = [n \in Slot |-> FALSE]
  /\ reactivate = [n \in Slot |-> 0]
  /\ remaining = [n \in Slot |-> 0]
  /\ consumers = [n \in Slot |-> {}]
  /\ binding = 0
  /\ bstate = "none"
  /\ wl = <<>>

(* Resolution: a required key must be bound active by someone else;
   self-exclusion makes the paper's prec-acyclicity effective (p. 49). *)
SatOK(n) == binding # 0 /\ binding # n /\ bstate = "ac"
Sat(n) == ~ReqDb[comp[n]] \/ SatOK(n)
ResolvedProv(n) == IF SatOK(n) THEN binding ELSE 0

(* Definition 53, p. 35 eq. (48) read at the concrete state. *)
TargetImpl(n) ==
  IF ~alive[n] \/ reactivate[n] = -1 THEN 0 ELSE ResolvedProv(n)

(* ---- orchestration ---------------------------------------------------- *)

Mount(n, c) ==
  (* O-Insert, p. 34, plus runtime::mount_locked. The single-source
     premise is enforced here: the engine diagnoses rather than enforces
     (on_diagnostic), and the conformance universe encodes the discipline
     the same way - see proof/README.md finding 1. *)
  /\ ~alive[n]
  /\ Provides[c] => ~ \E m \in Slot : alive[m] /\ Provides[comp[m]]
  /\ alive' = [alive EXCEPT ![n] = TRUE]
  /\ comp' = [comp EXCEPT ![n] = c]
  /\ state' = [state EXCEPT ![n] = "in"]
  /\ committed' = [committed EXCEPT ![n] = 0]
  /\ err' = [err EXCEPT ![n] = FALSE]
  /\ afailed' = [afailed EXCEPT ![n] = FALSE]
  /\ reactivate' = [reactivate EXCEPT ![n] = 0]
  /\ remaining' = [remaining EXCEPT ![n] = 0]
  /\ consumers' = [consumers EXCEPT ![n] = {}]
  /\ UNCHANGED <<binding, bstate, wl>>

RetireFlag(n) ==
  (* O-Retire, p. 34: tau_n := top. Araya's -1 flag is the monotone tau
     (Lemma 59(5)); it is sticky across cascades. runtime::retire. *)
  /\ alive[n] /\ reactivate[n] # -1
  /\ reactivate' = [reactivate EXCEPT ![n] = -1]
  /\ UNCHANGED <<alive, comp, state, committed, err, afailed, remaining,
                 consumers, binding, bstate, wl>>

(* ---- lifecycle -------------------------------------------------------- *)

EvaluateStart(n) ==
  (* L-Begin, p. 36 + runtime::evaluate/start_loading: an inactive fiber
     whose target is not bottom commits the resolution and enters loading.
     start_loading also clears the outcome (the revision retry, p. 56). *)
  /\ alive[n] /\ state[n] = "in" /\ reactivate[n] # -1
  /\ ~afailed[n] /\ Sat(n)
  /\ state' = [state EXCEPT ![n] = "ld"]
  /\ committed' = [committed EXCEPT ![n] = ResolvedProv(n)]
  /\ err' = [err EXCEPT ![n] = FALSE]
  /\ afailed' = [afailed EXCEPT ![n] = FALSE]
  /\ wl' = Append(wl, n)
  /\ UNCHANGED <<alive, comp, reactivate, remaining, consumers, binding,
                 bstate>>

EvaluateErr(n) ==
  (* A resolution failure is recorded once; nothing else moves
     (runtime::evaluate's else-branch). A stutter under alpha. *)
  /\ alive[n] /\ state[n] = "in" /\ reactivate[n] # -1
  /\ ~Sat(n) /\ ~err[n]
  /\ err' = [err EXCEPT ![n] = TRUE]
  /\ UNCHANGED <<alive, comp, state, committed, afailed, reactivate,
                 remaining, consumers, binding, bstate, wl>>

ProvidersActive(n) ==
  committed[n] = 0 \/ (binding = committed[n] /\ bstate = "ac")

ApplyComplete(n) ==
  (* L-Iter/L-Finish (p. 36) under Asynchrony (p. 55: a map in flight runs
     to completion) + runtime::on_apply_completed. Four outcomes:
     raise (LFail), cancelled (the diverted fiber lands - an in-flight
     iteration that was declined completes without publishing), stale
     (the target turned: route to unloading and let Finish apply the
     accumulator - the deactivation-after-landing of p. 55), and
     publish (L-Finish). *)
  /\ wl # <<>> /\ Head(wl) = n
  /\ wl' = Tail(wl)
  /\ \/ /\ (FAILS[comp[n]] \/ state[n] = "ul")
          (* raise or cancelled: nothing was installed (Corollary 69) *)
          /\ state' = [state EXCEPT ![n] = "in"]
          /\ afailed' = [afailed EXCEPT
               ![n] = FAILS[comp[n]] /\ state[n] = "ld"]
          /\ err' = [err EXCEPT ![n] = err[n] \/ (state[n] = "ul" /\ ~err[n])]
          /\ committed' = [committed EXCEPT ![n] = 0]
          /\ binding' = (IF binding = n /\ bstate = "ld" THEN 0 ELSE binding)
          /\ bstate' = (IF binding = n /\ bstate = "ld" THEN "none" ELSE bstate)
          /\ UNCHANGED <<alive, comp, reactivate, remaining, consumers>>
     \/ /\ state[n] = "ld" /\ ~FAILS[comp[n]]
          /\ (~ProvidersActive(n) \/ committed[n] # TargetImpl(n))
          (* stale: deactivates after the iteration lands, from Unloading
             (p. 55); the accumulator runs in Finish. The paper re-checks
             the target at completion in BOTH directions (L-Finish
             requires view = target, pp. 36/55-56); the engine's
             one-directional providers_still_active check is sufficient
             because its single-strand scheduling never lets a fiber's
             apply land after the binding map changed under it - the
             model is faithful to the paper, and the unreachable
             interleavings divert instead of publishing. *)
          /\ state' = [state EXCEPT ![n] = "ul"]
          /\ UNCHANGED <<alive, comp, committed, err, afailed, reactivate,
                         remaining, consumers, binding, bstate>>
     \/ /\ state[n] = "ld" /\ ~FAILS[comp[n]] /\ ProvidersActive(n)
          /\ reactivate[n] # -1
          /\ committed[n] = TargetImpl(n)
          (* publish: L-Finish, p. 36 + runtime::publish. The retirement
             guard mirrors the engine's synchronous retire-unload: retire
             flips the fiber to unloading in the same handler, so a
             retired fiber can never publish - the paper's L-Finish reads
             the same way through the tau premise of target. *)
          /\ state' = [state EXCEPT ![n] = "ac"]
          /\ binding' = (IF Provides[comp[n]] THEN n ELSE binding)
          /\ bstate' =
               (IF Provides[comp[n]]
                THEN (IF AvailInit[comp[n]] THEN "ac" ELSE "ld")
                ELSE bstate)
          /\ consumers' =
               (IF committed[n] = 0 THEN consumers
                ELSE [consumers EXCEPT
                       ![committed[n]] = consumers[committed[n]] \cup {n}])
          /\ UNCHANGED <<alive, comp, committed, err, afailed, reactivate,
                         remaining>>

UnloadEval(n) ==
  (* L-Leave, p. 37 + runtime::begin_unload on an active fiber: the
     marking, the binding's withdrawal from sigma_gamma (bstate ->
     retiring), the guard counter, and the cascade flags. Consumers
     unload through their own steps (the paper's rules are
     nondeterministic, p. 37; the C++ oracle keeps the exact order). *)
  /\ alive[n] /\ state[n] = "ac"
  /\ committed[n] # TargetImpl(n) \/ reactivate[n] = -1
  /\ state' = [state EXCEPT ![n] = "ul"]
  /\ bstate' = (IF binding = n THEN "rt" ELSE bstate)
  /\ reactivate' =
       [m \in Slot |->
          CASE m = n ->
                 IF reactivate[n] >= 0
                 THEN MaxOf(reactivate[n], IF Sat(n) THEN 1 ELSE 0)
                 ELSE reactivate[n]
            [] m \in consumers[n] ->
                 IF reactivate[m] >= 0 THEN MaxOf(reactivate[m], 1)
                 ELSE reactivate[m]
            [] OTHER -> reactivate[m]]
  /\ remaining' =
       [remaining EXCEPT
         ![n] = Cardinality({c \in consumers[n] :
                              alive[c] /\ state[c] # "in"})]
  /\ UNCHANGED <<alive, comp, committed, err, afailed, consumers, binding,
                 wl>>

UnloadDivert(n) ==
  (* L-Divert, p. 37, the landing alternative only (Asynchrony, p. 55):
     a fiber whose target turned mid-transition (or whose retirement
     arrived) routes into unloading; the pending apply completes
     cancelled in ApplyComplete. runtime::begin_unload on a loading
     fiber. *)
  /\ alive[n] /\ state[n] = "ld"
  /\ committed[n] # TargetImpl(n) \/ reactivate[n] = -1
  /\ state' = [state EXCEPT ![n] = "ul"]
  /\ UNCHANGED <<alive, comp, committed, err, afailed, reactivate,
                 remaining, consumers, binding, bstate, wl>>

Finish(n) ==
  (* L-Unload, p. 37 + runtime::finish_unload: the guard (Definition 54
     relied_n) holds because remaining[n] = 0 - every published consumer
     has finished. Applies the accumulator (withdraws the provision),
     discards the committed view, deregisters, and re-evaluates when the
     flag says so (the mutual chaining of reload and unload, p. 56). *)
  /\ alive[n] /\ state[n] = "ul" /\ remaining[n] = 0
  /\ n \notin {wl[i] : i \in 1..Len(wl)}
  (* a diverted fiber (L-Divert from loading) still has its apply in
     flight: it completes through ApplyComplete's cancelled branch, and
     only then is Finish enabled - runtime::finish_unload runs for
     active-origin unloads alone *)
  /\ state' = [state EXCEPT ![n] = "in"]
  /\ committed' = [committed EXCEPT ![n] = 0]
  /\ binding' = (IF binding = n THEN 0 ELSE binding)
  /\ bstate' = (IF binding = n THEN "none" ELSE bstate)
  /\ consumers' =
       IF committed[n] = 0 THEN consumers
       ELSE [consumers EXCEPT
              ![committed[n]] = consumers[committed[n]] \ {n}]
  /\ remaining' =
       [remaining EXCEPT
         ![committed[n]] =
           IF committed[n] # 0 /\ n \in consumers[committed[n]]
             THEN MaxOf(remaining[committed[n]] - 1, 0)
             ELSE remaining[committed[n]]]
  /\ reactivate' =
       [reactivate EXCEPT
         ![n] = IF reactivate[n] > 0 THEN 0 ELSE reactivate[n]]
  /\ UNCHANGED <<alive, comp, err, afailed, wl>>

Erase(n) ==
  (* O-Remove, p. 34 + runtime::retire's erase: a retired, inactive fiber
     holding no bindings is removed. The no-children premise is vacuous
     here (no instantiation in this universe, Definition 52 is v2). *)
  /\ alive[n] /\ state[n] = "in" /\ reactivate[n] = -1
  /\ alive' = [alive EXCEPT ![n] = FALSE]
  /\ consumers' = [consumers EXCEPT ![n] = {}]
  /\ UNCHANGED <<comp, state, committed, err, afailed, reactivate,
                 remaining, binding, bstate, wl>>

(* The availability extension, promotion only: an unavailable provider
   (binding = n, bstate = "ld" - it landed but has not yet published
   from the dependents' point of view) becomes available;
   runtime::signal_availability notifies the dependents, which move
   through their own steps. Deactivation is NOT expressible in the
   paper's lifecycle DAG (there is no ac -> ld edge), so making a
   service unavailable again means unloading the provider - the existing
   rules. Under alpha the provider lags in "ld" while unavailable, so
   this step IS the paper's L-Finish. *)
AvailabilityFlip(n) ==
  /\ alive[n] /\ state[n] = "ac" /\ reactivate[n] # -1
  /\ binding = n
  /\ bstate = "ld"
  /\ bstate' = "ac"
  /\ UNCHANGED <<alive, comp, state, committed, err, afailed, reactivate,
                 remaining, consumers, binding, wl>>

Next ==
  \/ \E n \in Slot : RetireFlag(n) \/ EvaluateStart(n) \/ EvaluateErr(n)
                     \/ ApplyComplete(n) \/ UnloadEval(n) \/ UnloadDivert(n)
                     \/ Finish(n) \/ Erase(n) \/ AvailabilityFlip(n)
  \/ \E n \in Slot, c \in Comp : Mount(n, c)

(* Internal steps: driven to completion by the engine once the
   environment (Mount / RetireFlag) stops moving. Weak fairness on the
   internal actions is the machine-side assumption behind Theorem 73. *)
Internal ==
  \E n \in Slot : ApplyComplete(n) \/ EvaluateStart(n) \/ EvaluateErr(n)
                  \/ UnloadEval(n) \/ UnloadDivert(n) \/ Finish(n)

Spec == Init /\ [][Next]_vars /\ WF_vars(Internal)

(* Quiescence: no pending applies, no transition in progress
   (Definition 53, eq. (49), read at the concrete state). *)
Quiescent ==
  /\ wl = <<>>
  /\ \A n \in Slot : alive[n] => state[n] \in {"in", "ac"}

=============================================================================
