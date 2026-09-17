(* PaperCalculus.v — the paper's calculus, Section 4.2, pp. 34-37, at the
   abstraction level the concrete machine (Machine.v) refines against. This
   transcribes proof/tla/PaperRules.tla. Every rule carries its page number;
   the metatheorems of Section 4.3 (Preservation Thm 64 p. 43, Temporal
   composability Thm 68 p. 46 + Corollary 69 p. 46, Spatial composability
   Thms 70/71 pp. 47-48, Progress Thm 73 p. 49, Confluence Thm 80 p. 54) are
   imported in Progress.v as axioms and transferred to the engine by the
   refinement of Refinement.v.

   State per fiber n (the paper's gamma, with F_gamma the registry):
     palive n    — n in dom(F_gamma)
     pcomp n     — the component n was inserted with (d_n, p_n, e_n;
                   Definition 48; immutable by Lemma 59(5))
     pstate n    — theta_n: StIn = Inactive, StLd = Reloading, StAc = Active,
                   StUl = Unloading (Definition 49)
     pview n     — omega_n, the committed view (Definition 49)
     pretired n  — tau_n, the retirement flag (O-Retire, p. 34)
     pok n       — the Failure-extension outcome (p. 56): false withholds
                   re-entry (L-Begin reads as requiring an error-free fiber)
     pbound      — sigma_gamma(db): the one shared realm (Section 4.1) with
                   the single key "example.db" read at it. p. 33 eq. (46) is
                   the union over ACTIVE fiber tables; with one key this is
                   the provider name or none. *)

Require Import Coq.Lists.List.
Require Import Coq.Bool.Bool.
Import ListNotations.
Require Import ArayaProof.Machine.

Section PaperCalculus.

Context {Component : Type}.

Variable provides : Component -> bool.
Variable req_db : Component -> bool.
Variable fails : Component -> bool.

Record AState := {
  palive : Slot -> bool;
  pcomp : Slot -> Component;
  pstate : Slot -> St4;
  pview : Slot -> option Slot;
  pretired : Slot -> bool;
  pok : Slot -> bool;
  pbound : option Slot;
}.

(* Whether the declared keys are satisfied (Section 3.2.2): a required key
   must be bound by an ACTIVE fiber other than n itself. The self-exclusion
   makes the paper's precedence-acyclicity assumption effective: the
   degenerate n prec n (p. 49) never resolves. *)
Definition sat_absb (a : AState) (n : Slot) : bool :=
  negb (req_db (pcomp a n))
  || match pbound a with
     | None => false
     | Some b => negb (Nat.eqb b n)
     end.

(* Definition 53, p. 35 eq. (48): the target view. Bottom when n ought not
   to be running at all: retired (tau_n), or unsatisfied; otherwise the
   provider of db. None is the EMPTY view (an optional declaration binding
   nothing yet), which is distinct from bottom: a fiber whose target turns
   to bottom mid-transition diverts even when it committed nothing
   (L-Divert's target != omega holds). *)
Inductive TV := TBot | TEmpty | TProv (n : Slot).

Definition target_abs (a : AState) (n : Slot) : TV :=
  if pretired a n || negb (sat_absb a n) then TBot
  else match pbound a with
       | None => TEmpty
       | Some b => TProv b
       end.

Definition pview_tv (a : AState) (n : Slot) : TV :=
  match pview a n with
  | None => TEmpty
  | Some m => TProv m
  end.

(* Definition 54, p. 36 eq. (50): relied upon — some other INSTALLED fiber's
   committed view names n. Read at the ACTIVE state alone: a loading
   consumer is not yet relied, since the provider may withdraw while the
   consumer's transition is in flight — Araya's consumer then reads its
   committed-view snapshot, and its completion-time re-check (the stale
   branch of ApplyComplete) diverts it, which is the paper's L-Divert under
   the Asynchrony extension (p. 55). This is the guard of L-Unload. *)
Definition relied_abs (a : AState) (n : Slot) : Prop :=
  exists m, palive a m = true /\ m <> n /\ pview a m = Some n
            /\ pstate a m = StAc.

(* ---- orchestration rules (gamma => delta), Section 4.2.1, p. 34 ------- *)

(* O-Insert, p. 34: n not in dom(F_gamma), the parent premise is vacuous
   (no instantiation, Definition 52 is v2), and the single-source premise
   — forall m in dom(F_gamma). p cap p_m = empty — read at the one key: at
   most one alive provider of db at a time. *)
Definition OInsert (n : Slot) (c : Component) (a a' : AState) : Prop :=
  palive a n = false
  /\ (provides c = true ->
      forall m, palive a m = true -> provides (pcomp a m) = false)
  /\ a' = {| palive := upd (palive a) n true;
             pcomp := upd (pcomp a) n c;
             pstate := upd (pstate a) n StIn;
             pview := upd (pview a) n None;
             pretired := upd (pretired a) n false;
             pok := upd (pok a) n true;
             pbound := pbound a |}.

(* O-Retire, p. 34: tau_n := top; unconditional on the fiber's state. *)
Definition ORetire (n : Slot) (a a' : AState) : Prop :=
  palive a n = true
  /\ pretired a n = false
  /\ a' = {| palive := palive a; pcomp := pcomp a; pstate := pstate a;
             pview := pview a; pretired := upd (pretired a) n true;
             pok := pok a; pbound := pbound a |}.

(* O-Remove, p. 34: tau_n = top, theta_n = Inactive, sigma_n = empty, no
   children. The no-children premise is vacuous in this universe. *)
Definition ORemove (n : Slot) (a a' : AState) : Prop :=
  palive a n = true
  /\ pretired a n = true
  /\ pstate a n = StIn
  /\ a' = {| palive := upd (palive a) n false; pcomp := pcomp a;
             pstate := pstate a; pview := pview a; pretired := pretired a;
             pok := pok a; pbound := pbound a |}.

(* ---- lifecycle rules (gamma --> delta), Section 4.2.2, pp. 36-37 ------ *)

(* L-Begin, p. 36: theta_n = Inactive, omega = target_n(gamma) != bottom.
   The Failure extension (p. 56) reads L-Begin as requiring an error-free
   fiber (pok). *)
Definition LBgin (n : Slot) (a a' : AState) : Prop :=
  palive a n = true
  /\ pstate a n = StIn
  /\ pok a n = true
  /\ pretired a n = false
  /\ sat_absb a n = true
  /\ a' = {| palive := palive a; pcomp := pcomp a;
             pstate := upd (pstate a) n StLd;
             pview := upd (pview a) n (pbound a);
             pretired := pretired a; pok := pok a; pbound := pbound a |}.

(* L-Iter (p. 36) is a stutter at this granularity: Araya's apply runs as a
   single coroutine body, the unit iterator (Definition 56, Lemma 57
   confinement) with no intermediate iteration boundary. *)

(* L-Finish, p. 36: the last iteration lands. Under the Asynchrony
   extension the target is re-checked at completion (pp. 55-56): the view
   must still be the target. *)
Definition LFinish (n : Slot) (a a' : AState) : Prop :=
  palive a n = true
  /\ pstate a n = StLd
  /\ pview_tv a n = target_abs a n
  /\ sat_absb a n = true
  /\ pretired a n = false
  /\ pok a n = true
  /\ a' = {| palive := palive a; pcomp := pcomp a;
             pstate := upd (pstate a) n StAc;
             pview := pview a; pretired := pretired a; pok := pok a;
             pbound := if provides (pcomp a n) then Some n else pbound a |}.

(* L-Fail: the Failure extension, p. 56. A raise exits Reloading by the
   route of an aborting L-Divert whose premise on the target view is
   dropped, arrives Inactive having installed nothing (Corollary 69), and
   writes the outcome; the outcome withholds re-entry. *)
Definition LFail (n : Slot) (a a' : AState) : Prop :=
  palive a n = true
  /\ pstate a n = StLd
  /\ a' = {| palive := palive a; pcomp := pcomp a;
             pstate := upd (pstate a) n StIn;
             pview := upd (pview a) n None;
             pretired := pretired a; pok := upd (pok a) n false;
             pbound := pbound a |}.

(* L-Divert, p. 37: theta_n = Reloading, target != omega. Only the landing
   alternative (Asynchrony, p. 55: a map in flight runs to completion
   whether or not it is still wanted). *)
Definition LDivert (n : Slot) (a a' : AState) : Prop :=
  palive a n = true
  /\ pstate a n = StLd
  /\ pview_tv a n <> target_abs a n
  /\ a' = {| palive := palive a; pcomp := pcomp a;
             pstate := upd (pstate a) n StUl;
             pview := pview a; pretired := pretired a; pok := pok a;
             pbound := pbound a |}.

(* L-Leave, p. 37: theta_n = Active, target != omega. The marking removes
   n's table from sigma_gamma (p. 37: "once L-Leave has marked n, its table
   leaves sigma_gamma"). *)
Definition LLeave (n : Slot) (a a' : AState) : Prop :=
  palive a n = true
  /\ pstate a n = StAc
  /\ pview_tv a n <> target_abs a n
  /\ a' = {| palive := palive a; pcomp := pcomp a;
             pstate := upd (pstate a) n StUl;
             pview := pview a; pretired := pretired a; pok := pok a;
             pbound := if opt_eqb slot_eqb (pbound a) (Some n)
                       then None else pbound a |}.

(* L-Unload, p. 37: theta_n = Unloading, not relied_n(gamma): the guard
   defers the withdrawal until every consumer that committed to n has gone
   (Theorem 70; Theorem 73 shows the guard always releases). Applies the
   accumulator and discards the committed view. *)
Definition LUnload (n : Slot) (a a' : AState) : Prop :=
  palive a n = true
  /\ pstate a n = StUl
  /\ ~ relied_abs a n
  /\ a' = {| palive := palive a; pcomp := pcomp a;
             pstate := upd (pstate a) n StIn;
             pview := upd (pview a) n None;
             pretired := pretired a; pok := pok a; pbound := pbound a |}.

Inductive ALabel :=
  | AOInsert | AORetire | AORemove
  | ALBgin | ALFinish | ALFail | ALDivert | ALLeave | ALUnload.

Inductive AStep : ALabel -> AState -> AState -> Prop :=
| ASOInsert : forall n c a a', OInsert n c a a' -> AStep AOInsert a a'
| ASORetire : forall n a a', ORetire n a a' -> AStep AORetire a a'
| ASORemove : forall n a a', ORemove n a a' -> AStep AORemove a a'
| ASLBgin : forall n a a', LBgin n a a' -> AStep ALBgin a a'
| ASLFinish : forall n a a', LFinish n a a' -> AStep ALFinish a a'
| ASLFail : forall n a a', LFail n a a' -> AStep ALFail a a'
| ASLDivert : forall n a a', LDivert n a a' -> AStep ALDivert a a'
| ASLLeave : forall n a a', LLeave n a a' -> AStep ALLeave a a'
| ASLUnload : forall n a a', LUnload n a a' -> AStep ALUnload a a'.

(* Definition 53, p. 35 eq. (49): quiet — every fiber settled at its target
   view, no transition left in progress. The Failure extension admits a
   failed fiber whatever its target view (p. 56). *)
Definition quiet_abs (a : AState) : Prop :=
  forall n, palive a n = true ->
    (pstate a n = StIn /\ pok a n = false)
    \/ match pstate a n with
       | StIn => target_abs a n = TBot
       | StAc => pview a n <> None /\ pview_tv a n = target_abs a n
                 /\ pok a n = true
       | _ => False
       end.

(* Definition 63, p. 43 (well-formedness), read at the single-key model.
   Clause 1 (parent pointers) is vacuous — no instantiation; clause 2 is
   the single-source discipline (at most one alive provider); clause 3 is
   total-valued views — here: an active fiber's view is a provider name;
   clause 4: a view names an installed (active) provider. *)
Definition well_formed_abs (a : AState) : Prop :=
  (forall n m, palive a n = true -> palive a m = true
               -> provides (pcomp a n) = true -> provides (pcomp a m) = true
               -> n = m)
  /\ (forall n, pstate a n = StAc
                -> exists p, pview a n = Some p)
  /\ (forall n p, pview a n = Some p -> pstate a n <> StIn
                  -> pstate a p = StAc /\ palive a p = true)
  /\ (forall n p, pstate a n <> StIn -> pview a n = Some p
                  -> pbound a = Some p).

End PaperCalculus.
