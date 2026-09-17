(* Machine.v — the concrete machine: Araya's runtime at the granularity of
   the paper's rules. This is a faithful transcription of the TLA+ model
   proof/tla/ArayaMachine.tla (the tier-2 authority), which the C++ oracle
   (proof/oracle/oracle.hpp) and the conformance suite pin to src/runtime.cpp
   (tier 1). Every action cites its paper rule and its runtime function.

   Paper reference: "A Programming Paradigm for Spatiotemporal
   Composability" (2608.25512v1.pdf). Rules: Section 4.2, pp. 34-37.
   Failure extension: Section 4.4, p. 56. Definition 53 (target view) p. 35,
   Definition 54 (relied guard) p. 36, Definition 49 (committed view) p. 31.

   Concrete state (the abstraction contract of proof/README.md):
     alive n      — the fiber record exists (runtime's fibers_ map)
     comp n       — the component, fixed at mount (paper: Lemma 59(5))
     state n      — In | Ld (loading) | Ac (active) | Ul (unloading)
     committed n  — the committed view omega_n (paper: Definition 49)
     err n        — a resolution error was recorded (stutter under alpha)
     afailed n    — the apply raised: the Failure outcome (p. 56)
     reactivate n — RRe: sticky retirement (tau, Lemma 59(5)); REval:
                    re-evaluate after unload (the mutual chaining of reload
                    and unload, p. 56); RNone otherwise
     remaining n  — the guard counter (Definition 54 relied_n implemented
                    as a counter; Theorem 73 shows it always releases)
     consumers n m — m published with n in its committed view
     binding      — the db binding under the one shared realm (Section 4.1)
     bstate       — the binding's provider state
     wl           — the pending-apply queue, FIFO in spawn order
     slots        — the universe of names (the paper's finite N,
                    Theorem 73's hypothesis made explicit) *)

Require Import Coq.Lists.List.
Require Import Coq.Bool.Bool.
Require Import Coq.Arith.PeanoNat.
Require Import Coq.Logic.FunctionalExtensionality.
Require Import Coq.Relations.Relation_Operators.
Import ListNotations.

(* Names (the paper's N): plain nats, drawn fresh at mount. *)

Definition Slot := nat.

(* Components: identifiers with a fixed signature. The conformance universe
   (tests/model_test.cpp, proof/conformance/conformance_test.cpp) supplies
   five; here the type is arbitrary, which is the generalization over TLC's
   two slots / five components. *)
Section Machine.

Context {Component : Type}.

Variable provides : Component -> bool.  (* declares the provision of db *)
Variable req_db : Component -> bool.    (* declares db as a required read *)
Variable fails : Component -> bool.     (* the apply always raises (p. 56) *)

(* The four lifecycle states; In is the paper's Inactive, Ld Reloading,
   Ac Active, Ul Unloading (Figure 1, p. 34). *)
Inductive St4 := StIn | StLd | StAc | StUl.

(* reactivate: -1 / 0 / 1 of ArayaMachine.tla *)
Inductive Re3 := RRe | RNone | REval.

(* the binding's provider state: none / loading / active / retiring *)
Inductive BSt := BNone | BLd | BAc | BRt.

Definition st4_eqb (a b : St4) : bool :=
  match a, b with
  | StIn, StIn | StLd, StLd | StAc, StAc | StUl, StUl => true
  | _, _ => false
  end.

Definition re3_eqb (a b : Re3) : bool :=
  match a, b with
  | RRe, RRe | RNone, RNone | REval, REval => true
  | _, _ => false
  end.

Definition bst_eqb (a b : BSt) : bool :=
  match a, b with
  | BNone, BNone | BLd, BLd | BAc, BAc | BRt, BRt => true
  | _, _ => false
  end.

Record State := {
  alive : Slot -> bool;
  comp : Slot -> Component;
  state : Slot -> St4;
  committed : Slot -> option Slot;
  err : Slot -> bool;
  afailed : Slot -> bool;
  reactivate : Slot -> Re3;
  remaining : Slot -> nat;
  consumers : Slot -> Slot -> bool;
  binding : option Slot;
  bstate : BSt;
  wl : list Slot;
  slots : list Slot;
}.

Definition upd {A : Type} (f : Slot -> A) (n : Slot) (v : A) : Slot -> A :=
  fun m => if Nat.eqb m n then v else f m.

Definition opt_eqb {A : Type} (ea : A -> A -> bool) (a b : option A) : bool :=
  match a, b with
  | None, None => true
  | Some x, Some y => ea x y
  | _, _ => false
  end.

Definition slot_eqb := Nat.eqb.

(* The MaxOf of ArayaMachine.tla over Re3: RRe is absorbing (tau is sticky,
   Lemma 59(5)); otherwise REval dominates. *)
Definition max_r3 (a b : Re3) : Re3 :=
  if re3_eqb a RRe then RRe
  else if re3_eqb b RRe then RRe
  else if (re3_eqb a REval) || (re3_eqb b REval) then REval
  else RNone.

(* Resolution (Section 3.2.2): a required key must be bound active by
   someone else; the self-exclusion makes the paper's prec-acyclicity
   (p. 49) effective. runtime::resolve. *)
Definition sat_okb (s : State) (n : Slot) : bool :=
  match binding s with
  | None => false
  | Some b =>
      match bstate s with
      | BAc => negb (Nat.eqb b n)
      | _ => false
      end
  end.

Definition satb (s : State) (n : Slot) : bool :=
  negb (req_db (comp s n)) || sat_okb s n.

Definition resolved_prov (s : State) (n : Slot) : option Slot :=
  if sat_okb s n then binding s else None.

(* Definition 53, p. 35 eq. (48) read at the concrete state: None doubles as
   bottom here (a retired fiber's target turns to bottom mid-transition and
   diverts even when it committed nothing — the TLA+ tier establishes the
   bottom/empty distinction in the abstraction). *)
Definition target_impl (s : State) (n : Slot) : option Slot :=
  if (alive s n) && negb (re3_eqb (reactivate s n) RRe)
  then resolved_prov s n
  else None.

(* runtime::providers_still_active (runtime.cpp): every provider in the
   committed view is still the binding and still active. *)
Definition providers_active (s : State) (n : Slot) : Prop :=
  committed s n = None
  \/ (binding s = committed s n /\ bstate s = BAc).

Definition consumers_add (cs : Slot -> Slot -> bool) (p : option Slot)
    (n : Slot) : Slot -> Slot -> bool :=
  match p with
  | None => cs
  | Some p' => fun q m => cs q m || ((Nat.eqb q p') && (Nat.eqb m n))
  end.

Definition consumers_remove (cs : Slot -> Slot -> bool) (p : option Slot)
    (n : Slot) : Slot -> Slot -> bool :=
  match p with
  | None => cs
  | Some p' => fun q m => cs q m && negb ((Nat.eqb q p') && (Nat.eqb m n))
  end.

(* Finish's remaining update: the consumer n withdraws from its provider
   (runtime.cpp: finish_unload decrements the provider's remaining). *)
Definition remaining_finish (s : State) (n : Slot) : Slot -> nat :=
  fun m =>
    match committed s n with
    | None => remaining s m
    | Some p =>
        if (Nat.eqb m p) && (consumers s p n)
        then Nat.sub (remaining s p) 1
        else remaining s m
    end.

(* UnloadEval's remaining recomputation (runtime.cpp: begin_unload counts
   the non-inactive consumers): the guard counter of Definition 54. *)
Definition count_installed (s : State) (n : Slot) : nat :=
  length
    (filter
       (fun c => consumers s n c && alive s c
                 && negb (st4_eqb (state s c) StIn))
       (slots s)).

(* UnloadEval's reactivate cascade (runtime.cpp: begin_unload notifies its
   consumers and re-evaluates the fiber itself when its resolution survives).
   RRe is sticky; REval dominates RNone (max_r3). *)
Definition reactivate_unload (s : State) (n : Slot) : Slot -> Re3 :=
  fun m =>
    if Nat.eqb m n
    then max_r3 (reactivate s n) (if satb s n then REval else RNone)
    else if consumers s n m
         then max_r3 (reactivate s m) REval
         else reactivate s m.

Definition same_state_alive_comp (s s' : State) : Prop :=
  alive s' = alive s /\ comp s' = comp s.

Inductive Label :=
  | LMount | LRetire | LEvalStart | LEvalErr | LApply
  | LUnloadEval | LUnloadDivert | LFinish | LErase.

Inductive Step : Label -> State -> State -> Prop :=

| SMount : forall n c s s',
    (* O-Insert, p. 34 + runtime::mount_locked. The single-source premise is
       enforced, not diagnosed, here: the conformance universe encodes the
       discipline the same way (proof/README.md finding 1). *)
    alive s n = false ->
    (provides c = true ->
     forall m, alive s m = true -> provides (comp s m) = false) ->
    ~ In n (slots s) ->
    s' = {| alive := upd (alive s) n true;
            comp := upd (comp s) n c;
            state := upd (state s) n StIn;
            committed := upd (committed s) n None;
            err := upd (err s) n false;
            afailed := upd (afailed s) n false;
            reactivate := upd (reactivate s) n RNone;
            remaining := upd (remaining s) n 0;
            consumers := upd (consumers s) n (fun _ => false);
            binding := binding s;
            bstate := bstate s;
            wl := wl s;
            slots := n :: slots s |} ->
    Step LMount s s'

| SRetire : forall n s s',
    (* O-Retire, p. 34: tau_n := top. Araya's RRe is the monotone tau
       (Lemma 59(5)); it is sticky across cascades. runtime::retire /
       retire_child / reconcile. *)
    alive s n = true ->
    reactivate s n <> RRe ->
    s' = {| alive := alive s; comp := comp s; state := state s;
            committed := committed s; err := err s; afailed := afailed s;
            reactivate := upd (reactivate s) n RRe;
            remaining := remaining s; consumers := consumers s;
            binding := binding s; bstate := bstate s; wl := wl s;
            slots := slots s |} ->
    Step LRetire s s'

| SEvalStart : forall n s s',
    (* L-Begin, p. 36 + runtime::evaluate / start_loading: an inactive fiber
       whose target is not bottom commits the resolution and enters loading.
       start_loading also clears the outcome (the revision retry, p. 56). *)
    alive s n = true ->
    state s n = StIn ->
    reactivate s n <> RRe ->
    afailed s n = false ->
    satb s n = true ->
    s' = {| alive := alive s; comp := comp s;
            state := upd (state s) n StLd;
            committed := upd (committed s) n (resolved_prov s n);
            err := upd (err s) n false;
            afailed := upd (afailed s) n false;
            reactivate := reactivate s; remaining := remaining s;
            consumers := consumers s; binding := binding s;
            bstate := bstate s; wl := wl s ++ [n]; slots := slots s |} ->
    Step LEvalStart s s'

| SEvalErr : forall n s s',
    (* A resolution failure is recorded once; nothing else moves
       (runtime::evaluate's else-branch). A stutter under alpha. *)
    alive s n = true ->
    state s n = StIn ->
    reactivate s n <> RRe ->
    satb s n = false ->
    err s n = false ->
    s' = {| alive := alive s; comp := comp s; state := state s;
            committed := committed s;
            err := upd (err s) n true; afailed := afailed s;
            reactivate := reactivate s; remaining := remaining s;
            consumers := consumers s; binding := binding s;
            bstate := bstate s; wl := wl s; slots := slots s |} ->
    Step LEvalErr s s'

| SApplyFail : forall n s s' rest,
    (* on_apply_completed (runtime.cpp): a raise or a cancellation — nothing
       was installed (Corollary 69). The raise writes the Failure outcome
       (p. 56), which withholds re-entry. *)
    wl s = n :: rest ->
    fails (comp s n) = true \/ state s n = StUl ->
    s' = {| alive := alive s; comp := comp s;
            state := upd (state s) n StIn;
            committed := upd (committed s) n None;
            err := upd (err s) n
              (err s n || (st4_eqb (state s n) StUl
                           && negb (err s n)));
            afailed := upd (afailed s) n
              (fails (comp s n) && st4_eqb (state s n) StLd);
            reactivate := reactivate s; remaining := remaining s;
            consumers := consumers s;
            binding := if (opt_eqb slot_eqb (binding s) (Some n)) && bst_eqb (bstate s) BLd
                       then None else binding s;
            bstate := if (opt_eqb slot_eqb (binding s) (Some n)) && bst_eqb (bstate s) BLd
                      then BNone else bstate s;
            wl := rest; slots := slots s |} ->
    Step LApply s s'

| SApplyStale : forall n s s' rest,
    (* L-Divert's landing alternative (Asynchrony, p. 55): the target turned
       while the apply was in flight; route to unloading and let Finish apply
       the accumulator (the deactivation-after-landing of p. 55). The paper
       re-checks the target at completion in BOTH directions (L-Finish
       requires view = target, pp. 36/55-56); the engine's one-directional
       providers_still_active check is sufficient because its single-strand
       scheduling never lets an apply land after the binding changed under
       it (proof/README.md finding 2). *)
    wl s = n :: rest ->
    state s n = StLd ->
    fails (comp s n) = false ->
    ~ providers_active s n \/ committed s n <> target_impl s n ->
    s' = {| alive := alive s; comp := comp s;
            state := upd (state s) n StUl;
            committed := committed s; err := err s; afailed := afailed s;
            reactivate := reactivate s; remaining := remaining s;
            consumers := consumers s; binding := binding s;
            bstate := bstate s; wl := rest; slots := slots s |} ->
    Step LApply s s'

| SApplyCancel : forall n s s' rest,
    (* A diverted fiber's apply lands cancelled: it completes without
       publishing (the paper's L-Unload on a vacuous guard, p. 37 — a fiber
       L-Divert takes out of its first transition provides nothing and
       appears in no committed view). *)
    wl s = n :: rest ->
    state s n = StUl ->
    s' = {| alive := alive s; comp := comp s;
            state := upd (state s) n StIn;
            committed := upd (committed s) n None;
            err := upd (err s) n
              (err s n || negb (err s n));
            afailed := afailed s;
            reactivate := reactivate s; remaining := remaining s;
            consumers := consumers s; binding := binding s;
            bstate := bstate s; wl := rest; slots := slots s |} ->
    Step LApply s s'

| SApplyPublish : forall n s s' rest,
    (* L-Finish, p. 36 + runtime::publish. The retirement guard mirrors the
       engine's synchronous retire-unload: retire flips the fiber to
       unloading in the same handler, so a retired fiber can never publish —
       the paper's L-Finish reads the same way through the tau premise of
       target. *)
    wl s = n :: rest ->
    state s n = StLd ->
    fails (comp s n) = false ->
    providers_active s n ->
    reactivate s n <> RRe ->
    committed s n = target_impl s n ->
    s' = {| alive := alive s; comp := comp s;
            state := upd (state s) n StAc;
            committed := committed s; err := err s; afailed := afailed s;
            reactivate := reactivate s; remaining := remaining s;
            consumers := consumers_add (consumers s) (committed s n) n;
            binding := if provides (comp s n) then Some n else binding s;
            bstate := if provides (comp s n) then BAc else bstate s;
            wl := rest; slots := slots s |} ->
    Step LApply s s'

| SUnloadEval : forall n s s',
    (* L-Leave, p. 37 + runtime::begin_unload on an active fiber: the
       marking, the binding's withdrawal from sigma_gamma (bstate ->
       retiring), the guard counter, and the cascade flags. *)
    alive s n = true ->
    state s n = StAc ->
    committed s n <> target_impl s n \/ reactivate s n = RRe ->
    s' = {| alive := alive s; comp := comp s;
            state := upd (state s) n StUl;
            committed := committed s; err := err s; afailed := afailed s;
            reactivate := reactivate_unload s n;
            remaining := upd (remaining s) n (count_installed s n);
            consumers := consumers s;
            binding := binding s;
            bstate := if opt_eqb slot_eqb (binding s) (Some n) then BRt else bstate s;
            wl := wl s; slots := slots s |} ->
    Step LUnloadEval s s'

| SUnloadDivert : forall n s s',
    (* L-Divert, p. 37, the landing alternative only (Asynchrony, p. 55):
       a fiber whose target turned mid-transition routes into unloading; the
       pending apply completes cancelled in SApplyCancel.
       runtime::begin_unload on a loading fiber. *)
    alive s n = true ->
    state s n = StLd ->
    committed s n <> target_impl s n \/ reactivate s n = RRe ->
    s' = {| alive := alive s; comp := comp s;
            state := upd (state s) n StUl;
            committed := committed s; err := err s; afailed := afailed s;
            reactivate := reactivate s; remaining := remaining s;
            consumers := consumers s; binding := binding s;
            bstate := bstate s; wl := wl s; slots := slots s |} ->
    Step LUnloadDivert s s'

| SFinish : forall n s s',
    (* L-Unload, p. 37 + runtime::finish_unload: the guard (Definition 54
       relied_n) holds because remaining[n] = 0 — every published consumer
       has finished. A diverted fiber still has its apply in flight: it
       completes through SApplyCancel, and only then is Finish enabled
       (runtime::finish_unload runs for active-origin unloads alone). *)
    alive s n = true ->
    state s n = StUl ->
    remaining s n = 0 ->
    ~ In n (wl s) ->
    s' = {| alive := alive s; comp := comp s;
            state := upd (state s) n StIn;
            committed := upd (committed s) n None;
            err := err s; afailed := afailed s;
            reactivate := upd (reactivate s) n
              (if re3_eqb (reactivate s n) REval then RNone
               else reactivate s n);
            remaining := remaining_finish s n;
            consumers := consumers_remove (consumers s) (committed s n) n;
            binding := if opt_eqb slot_eqb (binding s) (Some n) then None else binding s;
            bstate := if opt_eqb slot_eqb (binding s) (Some n) then BNone else bstate s;
            wl := wl s; slots := slots s |} ->
    Step LFinish s s'

| SErase : forall n s s',
    (* O-Remove, p. 34 + runtime::retire's erase: a retired, inactive fiber
       holding no bindings is removed. The no-children premise is vacuous
       here (no instantiation in this universe, Definition 52 is v2). *)
    alive s n = true ->
    state s n = StIn ->
    reactivate s n = RRe ->
    s' = {| alive := upd (alive s) n false;
            comp := comp s; state := state s; committed := committed s;
            err := err s; afailed := afailed s; reactivate := reactivate s;
            remaining := remaining s;
            consumers := upd (consumers s) n (fun _ => false);
            binding := binding s; bstate := bstate s; wl := wl s;
            slots := slots s |} ->
    Step LErase s s'.

Definition Init (s : State) : Prop :=
  alive s = (fun _ => false)
  /\ state s = (fun _ => StIn)
  /\ committed s = (fun _ => None)
  /\ err s = (fun _ => false)
  /\ afailed s = (fun _ => false)
  /\ reactivate s = (fun _ => RNone)
  /\ remaining s = (fun _ => 0)
  /\ consumers s = (fun _ _ => false)
  /\ binding s = None
  /\ bstate s = BNone
  /\ wl s = []
  /\ slots s = [].

(* Reachability: everything the conformance suite and the model checker
   start from, closed under the transition relation. *)
Definition Reach (s : State) : Prop :=
  exists s0, Init s0 /\ clos_refl_trans _ (fun x y => exists l, Step l x y) s0 s.

End Machine.
