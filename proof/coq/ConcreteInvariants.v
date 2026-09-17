(* ConcreteInvariants.v — the invariants of the concrete machine, proven
   directly over Step for ALL instances (the TLA+ tier checked the same
   statements exhaustively on two slots / five components). These are the
   contracts an optimization of src/runtime.cpp must preserve, keyed to the
   paper:

     I1  state StIn -> committed None          (Def. 49: Inactive carries
                                                no committed view)
     I2  state StLd -> binding <> Some n        (a fiber whose apply is in
                                                flight has not published)
     I18 binding = Some n -> n alive, provides,
         (StAc,BAc) or (StUl,BRt)               (p. 33 eq. (46): sigma_gamma
                                                is the union over ACTIVE
                                                providers' tables)
     I5  committed m = Some k /\ state m = StAc -> binding = Some k /\
         alive k (Thm 70(1) weakened to the model's interleavings: the
         engine's begin_unload cascades to consumers synchronously, the
         model leaves the cascade to the next step, so bstate may be BRt
         and k may be StUl in between — the refinement only needs the
         binding to survive)
     I6  single provider                       (O-Insert premise p. 34)
     I7  consumers-index exactness             (Def. 54 relied_n, the
                                                counter implementation)
     I8  state StUl -> remaining = installed count (Def. 54 guard; Thm 73
                                                shows it releases)
     I9/I10/I23 wl bookkeeping                 (pending applies, FIFO)
     I11 afailed -> StIn                       (Failure p. 56: the outcome
                                                withholds re-entry)
     I12 In n wl -> binding <> Some n
     I13-I16 slots/universe bookkeeping        (Thm 73's finite-N
                                                hypothesis)
     I19 state StLd -> no consumers of n       (a loading fiber has not
                                                published)
     I20 committed n <> Some n                 (self-exclusion, p. 49)
     I21 In n wl -> no fiber publishes against n
     I22 state StIn -> no fiber publishes against n
     I25 state n <> StUl -> remaining n = 0    (the counter is armed only
                                                while the fiber unloads) *)

Require Import Coq.Lists.List.
Require Import Coq.Bool.Bool.
Require Import Coq.Arith.PeanoNat.
Require Import Coq.Logic.FunctionalExtensionality.
Require Import Coq.micromega.Lia.
Import ListNotations.
Require Import ArayaProof.Machine.

Section ConcreteInvariants.

Context {Component : Type}.

Variable provides : Component -> bool.
Variable req_db : Component -> bool.
Variable fails : Component -> bool.

Definition CStep := Step provides req_db fails.
Definition CInit := Init (Component := Component).
Definition CCount (s : State) (n : Slot) : nat :=
  count_installed (Component := Component) s n.

(* ---- helpers ---------------------------------------------------------- *)

Lemma upd_eq : forall {A : Type} (f : Slot -> A) n v, upd f n v n = v.
Proof. intros. unfold upd. rewrite Nat.eqb_refl. reflexivity. Qed.

Lemma upd_ne : forall {A : Type} (f : Slot -> A) n m v, m <> n -> upd f n v m = f m.
Proof. intros. unfold upd. destruct (Nat.eqb m n) eqn:E.
  - apply Nat.eqb_eq in E. congruence.
  - reflexivity.
Qed.

Lemma opt_eqb_true : forall a b, opt_eqb slot_eqb a b = true -> a = b.
Proof.
  intros [x|] [y|] H; try discriminate.
  - unfold opt_eqb, slot_eqb in H. apply Nat.eqb_eq in H. congruence.
  - reflexivity.
Qed.

Lemma st4_eqb_true : forall a b, st4_eqb a b = true -> a = b.
Proof.
  intros [| | | ] [| | | ] H; try discriminate; reflexivity.
Qed.

Lemma bst_eqb_true : forall a b, bst_eqb a b = true -> a = b.
Proof.
  intros [| | | ] [| | | ] H; try discriminate; reflexivity.
Qed.

Lemma re3_eqb_true : forall a b, re3_eqb a b = true -> a = b.
Proof.
  intros [| | ] [| | ] H; try discriminate; reflexivity.
Qed.

(* Counting over the slots universe (the paper's finite N, Thm 73). *)

Lemma count_zero : forall s n,
    (forall c, consumers s n c = false \/ alive s c = false
               \/ state s c = StIn) ->
    CCount s n = 0.
Proof.
  intros s n H. unfold CCount, count_installed. induction (slots s) as [|c l IHl].
  - reflexivity.
  - simpl. destruct (consumers s n c) eqn:E1.
    + destruct (alive s c) eqn:E2.
      * destruct (st4_eqb (state s c) StIn) eqn:E3.
        -- apply IHl.
        -- specialize (H c). destruct H as [H|[H|H]]; try congruence.
           rewrite H in E3. simpl in E3. discriminate.
      * apply IHl.
    + apply IHl.
Qed.

Lemma count_ge1 : forall s n c,
    consumers s n c = true -> alive s c = true -> state s c <> StIn ->
    In c (slots s) -> 1 <= CCount s n.
Proof.
  intros s n c Hc Ha Hs Hin. unfold CCount, count_installed.
  induction (slots s) as [|d l IHl]; [easy|]. simpl in Hin. destruct Hin as [Hin|Hin].
  - subst d. simpl. rewrite Hc, Ha. simpl.
    destruct (st4_eqb (state s c) StIn) eqn:E. apply st4_eqb_true in E. congruence.
    simpl. lia.
  - simpl. destruct (consumers s n d) eqn:E1; simpl.
    * destruct (alive s d) eqn:E2; simpl.
      -- destruct (st4_eqb (state s d) StIn) eqn:E3; simpl.
         ++ apply IHl; exact Hin.
         ++ lia.
      -- apply IHl; exact Hin.
    * apply IHl; exact Hin.
Qed.

Lemma count_set_eq : forall s s' n,
    slots s' = slots s ->
    (forall c, In c (slots s) ->
       (consumers s' n c && alive s' c && negb (st4_eqb (state s' c) StIn))
       = (consumers s n c && alive s c && negb (st4_eqb (state s c) StIn))) ->
    CCount s' n = CCount s n.
Proof.
  intros s s' n Hsl Hf. unfold CCount, count_installed. rewrite Hsl. clear Hsl.
  induction (slots s) as [|c l IHl]; auto.
  simpl. rewrite Hf by (simpl; auto).
  destruct (consumers s n c && alive s c && negb (st4_eqb (state s c) StIn)) eqn:E.
  - simpl. f_equal. apply IHl. intros d Hd. apply Hf. simpl. auto.
  - simpl. apply IHl. intros d Hd. apply Hf. simpl. auto.
Qed.

Lemma filter_ext : forall (l : list Slot) (P P' : Slot -> bool),
    (forall c, In c l -> P c = P' c) -> filter P l = filter P' l.
Proof.
  intros l P P' H. induction l as [|c l IHl]; auto.
  simpl. rewrite H by (simpl; auto).
  rewrite IHl by (intros d Hd; apply H; simpl; auto).
  reflexivity.
Qed.

Lemma nodup_mid : forall (l1 l2 : list Slot) (x : Slot),
    NoDup (l1 ++ x :: l2) -> ~ In x l1.
Proof.
  induction l1 as [|a l1 IH]; intros l2 x Hnd Hx.
  - easy.
  - simpl in Hnd. inversion Hnd as [|? ? Hnin Hnd']; simpl in Hx.
    destruct Hx as [Hx|Hx].
    + subst a. apply Hnin. apply in_or_app. right. simpl. auto.
    + apply (IH l2 x Hnd' Hx).
Qed.

Lemma nodup_tail : forall (l1 l2 : list Slot) (x : Slot),
    NoDup (l1 ++ x :: l2) -> ~ In x l2.
Proof.
  intros l1 l2 x Hnd Hx.
  pose proof (NoDup_remove_2 l1 l2 x Hnd) as Hnin.
  apply Hnin. apply in_or_app. right. exact Hx.
Qed.

Lemma filter_remove_one : forall (l : list Slot) (P P' : Slot -> bool) (x : Slot),
    NoDup l -> In x l -> P x = true -> P' x = false ->
    (forall c, In c l -> c <> x -> P c = P' c) ->
    length (filter P l) = S (length (filter P' l)).
Proof.
  intros l P P' x Hnd Hin Hpx Hp'x Hf.
  destruct (in_split x l Hin) as [l1 [l2 Hl]].
  rewrite Hl in *. rewrite filter_app, filter_app.
  simpl. rewrite Hpx, Hp'x. simpl.
  rewrite (filter_ext l1 P P') by
    (intros c Hc; apply Hf; [apply in_or_app; left; exact Hc
      | intro Hcx; subst c; apply (nodup_mid l1 l2 x Hnd) in Hc; easy]).
  rewrite (filter_ext l2 P P') by
    (intros c Hc; apply Hf; [apply in_or_app; right; simpl; auto
      | intro Hcx; subst c; apply (nodup_tail l1 l2 x Hnd) in Hc; easy]).
  rewrite !app_length. simpl. lia.
Qed.

Lemma count_remove_one_succ : forall s s' p n,
    NoDup (slots s) ->
    consumers s p n = true -> alive s n = true -> state s n <> StIn ->
    In n (slots s) ->
    slots s' = slots s ->
    alive s' = alive s ->
    consumers s' p n = false ->
    (forall q m, q <> p \/ m <> n -> consumers s' q m = consumers s q m) ->
    (forall c, c <> n -> state s' c = state s c) ->
    state s' n = StIn ->
    S (CCount s' p) = CCount s p.
Proof.
  intros s s' p n Hnd Hc Ha Hs Hin Hsl Hal Hcn Hother Hst Hstn.
  unfold CCount, count_installed. rewrite Hsl.
  assert (Hpn : (consumers s p n && alive s n && negb (st4_eqb (state s n) StIn)) = true).
  { rewrite Hc, Ha. simpl. destruct (state s n) eqn:E; simpl; try reflexivity.
    easy. }
  assert (Hpn' : (consumers s' p n && alive s' n && negb (st4_eqb (state s' n) StIn)) = false).
  { rewrite Hcn, Hstn. simpl. reflexivity. }
  assert (Hf : forall c, In c (slots s) -> c <> n ->
      (consumers s p c && alive s c && negb (st4_eqb (state s c) StIn)) =
      (consumers s' p c && alive s' c && negb (st4_eqb (state s' c) StIn))).
  { intros c Hcin Hcn'. pose proof (Hother p c (or_intror Hcn')) as Hpc.
    rewrite Hpc. rewrite Hal. rewrite Hst by exact Hcn'. reflexivity. }
  rewrite (filter_remove_one (slots s)
      (fun c => consumers s p c && alive s c && negb (st4_eqb (state s c) StIn))
      (fun c => consumers s' p c && alive s' c && negb (st4_eqb (state s' c) StIn))
      n Hnd Hin Hpn Hpn' Hf).
  reflexivity.
Qed.

Lemma count_remove_one : forall s s' p n,
    NoDup (slots s) ->
    consumers s p n = true -> alive s n = true -> state s n <> StIn ->
    In n (slots s) ->
    slots s' = slots s ->
    alive s' = alive s ->
    consumers s' p n = false ->
    (forall q m, q <> p \/ m <> n -> consumers s' q m = consumers s q m) ->
    (forall c, c <> n -> state s' c = state s c) ->
    state s' n = StIn ->
    CCount s' p = CCount s p - 1.
Proof.
  intros. pose proof (count_remove_one_succ s s' p n H H0 H1 H2 H3 H4 H5 H6 H7 H8 H9).
  lia.
Qed.

(* ---- the invariant ---------------------------------------------------- *)

Definition Good (s : State) : Prop :=
  (forall n, state s n = StIn -> committed s n = None)                        (* I1 *)
  /\ (forall n, state s n = StLd -> binding s <> Some n)                      (* I2 *)
  /\ (forall n, binding s = Some n -> alive s n = true
        /\ provides (comp s n) = true
        /\ ((state s n = StAc /\ bstate s = BAc)
            \/ (state s n = StUl /\ bstate s = BRt)))                         (* I18 *)
  /\ (forall m k, committed s m = Some k -> state s m = StAc
        -> binding s = Some k /\ alive s k = true)                            (* I5 *)
  /\ (forall n m, alive s n = true -> alive s m = true
        -> provides (comp s n) = true -> provides (comp s m) = true
        -> n = m)                                                             (* I6 *)
  /\ (forall p n, consumers s p n = true -> alive s n = true
        /\ alive s p = true /\ committed s n = Some p /\ state s n <> StIn)   (* I7a *)
  /\ (forall n p, committed s n = Some p -> state s n = StAc
        -> consumers s p n = true)                                            (* I7b *)
  /\ (forall n, state s n = StUl -> remaining s n = CCount s n)      (* I8 *)
  /\ (forall n, In n (wl s) -> alive s n = true
        /\ (state s n = StLd \/ state s n = StUl))                            (* I9 *)
  /\ (forall n, alive s n = true -> state s n = StLd -> In n (wl s))          (* I10 *)
  /\ (forall n, afailed s n = true -> state s n = StIn)                       (* I11 *)
  /\ (forall n, In n (wl s) -> binding s <> Some n)                           (* I12 *)
  /\ (forall n, alive s n = true -> In n (slots s))                           (* I13 *)
  /\ (forall n, In n (wl s) -> In n (slots s))                                (* I14 *)
  /\ (forall p n, consumers s p n = true
        -> In n (slots s) /\ In p (slots s))                                  (* I15 *)
  /\ NoDup (slots s)                                                          (* I16 *)
  /\ (forall n, state s n = StLd -> forall c, consumers s n c = false)        (* I19 *)
  /\ (forall n, committed s n <> Some n)                                      (* I20 *)
  /\ (forall m n, In n (wl s) -> consumers s m n = false)                     (* I21 *)
  /\ (forall m n, state s n = StIn -> consumers s m n = false)                (* I22 *)
  /\ NoDup (wl s)                                                             (* I23 *)
  /\ (forall n, state s n <> StUl -> remaining s n = 0).                      (* I25 *)

Ltac unpack Hg :=
  unfold Good in Hg; simpl in Hg;
  destruct Hg as [I1 [I2 [I18 [I5 [I6 [I7a [I7b [I8 [I9 [I10 [I11 [I12
    [I13 [I14 [I15 [I16 [I19 [I20 [I21 [I22 [I23 I25]]]]]]]]]]]]]]]]]]]]].

Ltac good_split :=
  unfold Good; simpl; repeat split.

(* The clause dispatcher: each goal is one conjunct of Good at the
   post-state, recognized by shape and closed by the matching pre-state
   clause. Order-independent, so the per-constructor proofs only add the
   genuinely non-local clauses (the guard counter I8). *)
(* Clause projections: the refinement and progress proofs use these. *)

Lemma good_I1 : forall s, Good s -> forall n, state s n = StIn -> committed s n = None.
Proof. intros s Hg. unpack Hg. exact I1. Qed.

Lemma good_I2 : forall s, Good s -> forall n, state s n = StLd -> binding s <> Some n.
Proof. intros s Hg. unpack Hg. exact I2. Qed.

Lemma good_I18 : forall s, Good s -> forall n, binding s = Some n ->
    alive s n = true /\ provides (comp s n) = true
    /\ ((state s n = StAc /\ bstate s = BAc)
        \/ (state s n = StUl /\ bstate s = BRt)).
Proof. intros s Hg. unpack Hg. exact I18. Qed.

Lemma good_I5 : forall s, Good s -> forall m k,
    committed s m = Some k -> state s m = StAc
    -> binding s = Some k /\ alive s k = true.
Proof. intros s Hg. unpack Hg. exact I5. Qed.

Lemma good_I7a : forall s, Good s -> forall p n, consumers s p n = true ->
    alive s n = true /\ alive s p = true
    /\ committed s n = Some p /\ state s n <> StIn.
Proof. intros s Hg. unpack Hg. exact I7a. Qed.

Lemma good_I7b : forall s, Good s -> forall n p,
    committed s n = Some p -> state s n = StAc -> consumers s p n = true.
Proof. intros s Hg. unpack Hg. exact I7b. Qed.

Lemma good_I6 : forall s, Good s -> forall n m,
    alive s n = true -> alive s m = true
    -> provides (comp s n) = true -> provides (comp s m) = true -> n = m.
Proof. intros s Hg. unpack Hg. exact I6. Qed.

Lemma good_I10 : forall s, Good s -> forall n,
    alive s n = true -> state s n = StLd -> In n (wl s).
Proof. intros s Hg. unpack Hg. exact I10. Qed.

Lemma good_I13 : forall s, Good s -> forall n, alive s n = true -> In n (slots s).
Proof. intros s Hg. unpack Hg. exact I13. Qed.

Lemma good_I14 : forall s, Good s -> forall n, In n (wl s) -> In n (slots s).
Proof. intros s Hg. unpack Hg. exact I14. Qed.

Lemma good_I19 : forall s, Good s -> forall n,
    state s n = StLd -> forall c, consumers s n c = false.
Proof. intros s Hg. unpack Hg. exact I19. Qed.

Lemma good_I22 : forall s, Good s -> forall m n,
    state s n = StIn -> consumers s m n = false.
Proof. intros s Hg. unpack Hg. exact I22. Qed.

Lemma good_I23 : forall s, Good s -> NoDup (wl s).
Proof. intros s Hg. unpack Hg. exact I23. Qed.

Lemma good_I25 : forall s, Good s -> forall n,
    state s n <> StUl -> remaining s n = 0.
Proof. intros s Hg. unpack Hg. exact I25. Qed.

Lemma good_I8 : forall s, Good s -> forall n,
    state s n = StUl -> remaining s n = count_installed s n.
Proof. intros s Hg. unpack Hg. exact I8. Qed.

Lemma good_I9 : forall s, Good s -> forall n, In n (wl s) ->
    alive s n = true /\ (state s n = StLd \/ state s n = StUl).
Proof. intros s Hg. unpack Hg. exact I9. Qed.

Lemma good_I11 : forall s, Good s -> forall n, afailed s n = true -> state s n = StIn.
Proof. intros s Hg. unpack Hg. exact I11. Qed.

Lemma good_I12 : forall s, Good s -> forall n, In n (wl s) -> binding s <> Some n.
Proof. intros s Hg. unpack Hg. exact I12. Qed.

Lemma good_I15 : forall s, Good s -> forall p n, consumers s p n = true ->
    In n (slots s) /\ In p (slots s).
Proof. intros s Hg. unpack Hg. exact I15. Qed.

Lemma good_I16 : forall s, Good s -> NoDup (slots s).
Proof. intros s Hg. unpack Hg. exact I16. Qed.

Lemma good_I20 : forall s, Good s -> forall n, committed s n <> Some n.
Proof. intros s Hg. unpack Hg. exact I20. Qed.

Lemma good_I21 : forall s, Good s -> forall m n, In n (wl s) -> consumers s m n = false.
Proof. intros s Hg. unpack Hg. exact I21. Qed.


Ltac solve_clause :=
  intros; simpl in *;
  repeat (rewrite upd_eq in * || rewrite upd_ne in * by congruence);
  repeat match goal with
  | H : opt_eqb slot_eqb ?a ?b = true |- _ => apply opt_eqb_true in H
  | H : st4_eqb ?a ?b = true |- _ => apply st4_eqb_true in H
  | H : bst_eqb ?a ?b = true |- _ => apply bst_eqb_true in H
  | H : re3_eqb ?a ?b = true |- _ => apply re3_eqb_true in H
  | H : Nat.eqb ?a ?b = true |- _ => apply Nat.eqb_eq in H
  end; subst;
  try congruence;
  try (eauto 8).

Lemma init_good : forall s, CInit s -> Good s.
Proof.
  intros s Hi. unfold CInit, Init in Hi. simpl in Hi.
  destruct Hi as [Ha [Hst [Hc [He [Haf [Hr [Hrem [Hcon [Hb [Hbs [Hwl Hsl]]]]]]]]]]].
  good_split.
  all: try rewrite Ha in *; try rewrite Hst in *; try rewrite Hc in *;
      try rewrite He in *; try rewrite Haf in *; try rewrite Hr in *;
      try rewrite Hrem in *; try rewrite Hcon in *; try rewrite Hb in *;
      try rewrite Hbs in *; try rewrite Hwl in *; try rewrite Hsl in *.
  all: try (intros; simpl in *; discriminate).
  all: try (intros; simpl in *; easy).
  all: try (intros; simpl in *; reflexivity).
  all: try (intros; simpl in *; easy).
  all: try (intros; simpl in *; contradiction).
  all: try constructor.
Qed.

Lemma smount_good : forall s s', Good s -> CStep LMount s s' -> Good s'.
Proof.
  intros s s' Hg Hs. inversion Hs; subst. unpack Hg. good_split.
  all: try solve_clause.
  (* I16 *) intros. simpl. constructor; [congruence | apply I16].
  (* I23 *) intros. apply I23.
Qed.

Lemma sretire_good : forall s s', Good s -> CStep LRetire s s' -> Good s'.
Proof.
  intros s s' Hg Hs. inversion Hs; subst. unpack Hg. good_split.
  all: try solve_clause.
Qed.

(* ---- count preservation (I8's maintenance), per constructor ----------- *)

Ltac count_seteq :=
  match goal with
  | |- CCount ?t ?m = CCount ?s ?m =>
      apply (count_set_eq s t m)
  end.

Lemma sevalstart_count : forall s s' m,
    Good s -> CStep LEvalStart s s' ->
    CCount s' m = CCount s m.
Proof.
  intros s s' m Hg Hs.
  inversion Hs as [n s0 s1 Halive Hstate Hre Haf Hsat Heq]; subst. unpack Hg.
  count_seteq. simpl. split; [reflexivity|].
  intros c Hcin. simpl.
  destruct (Nat.eqb_spec c n).
  - subst c. f_equal. apply I22. exact Hstate.
  - rewrite upd_ne by easy. reflexivity.
Qed.

Lemma sevalerr_count : forall s s' m,
    Good s -> CStep LEvalErr s s' ->
    CCount s' m = CCount s m.
Proof.
  intros s s' m Hg Hs. inversion Hs; subst.
  count_seteq. simpl. split; [reflexivity|].
  intros c Hcin. reflexivity.
Qed.

Lemma sapply_count : forall s s' m,
    Good s -> CStep LApply s s' -> state s m = StUl ->
    CCount s' m = CCount s m.
Proof.
  intros s s' m Hg Hs Hsm.
  inversion Hs as
    [n s0 s1 rest Hwl Hfp Heq
    |n s0 s1 rest Hwl Hld Hnf Hstl Heq
    |n s0 s1 rest Hwl Hul Heq
    |n s0 s1 rest Hwl Hld Hnf Hpa Hre Hc Heq]; subst. unpack Hg.
  all: count_seteq; simpl; split; [reflexivity|]; intros c Hcin; simpl.
  (* SApplyFail *)
  - destruct (Nat.eqb_spec c n).
    + subst c. f_equal. apply I21 with (m := m). rewrite Hwl. simpl. auto.
    + rewrite upd_ne by easy. reflexivity.
  (* SApplyStale: Ld -> Ul, both non-inactive *)
  - destruct (Nat.eqb_spec c n).
    + subst c. f_equal. simpl. rewrite Hld. reflexivity.
    + rewrite upd_ne by easy. reflexivity.
  (* SApplyCancel *)
  - destruct (Nat.eqb_spec c n).
    + subst c. f_equal. apply I21 with (m := m). rewrite Hwl. simpl. auto.
    + rewrite upd_ne by easy. reflexivity.
  (* SApplyPublish *)
  - destruct (Nat.eqb_spec c n).
    + subst c. unfold consumers_add. destruct (committed s n) as [p|] eqn:Ecomm.
      * simpl. destruct (Nat.eqb_spec m p).
        -- subst m. exfalso. unfold providers_active in Hpa. rewrite Ecomm in Hpa.
           destruct Hpa as [Hc0|[Hbind Hbs]]; [discriminate|].
           apply I18 in Hbind. destruct Hbind as [_ [_ [Hd|Hd]]].
           ++ destruct Hd as [Hst' _]. congruence.
           ++ destruct Hd as [_ Hbs']. congruence.
        -- simpl. rewrite (I21 m n) by (rewrite Hwl; simpl; auto). reflexivity.
      * simpl. rewrite (I21 m n) by (rewrite Hwl; simpl; auto). reflexivity.
    + rewrite upd_ne by easy. reflexivity.
Qed.

Lemma sunloadeval_count : forall s s' m,
    Good s -> CStep LUnloadEval s s' -> state s m = StUl ->
    CCount s' m = CCount s m.
Proof.
  intros s s' m Hg Hs Hsm.
  inversion Hs as [n s0 s1 Halive Hstate Hstl Heq]; subst. unpack Hg.
  count_seteq. simpl. split; [reflexivity|].
  intros c Hcin. simpl.
  destruct (Nat.eqb_spec c n).
  - subst c. f_equal.
    assert (Hnn : consumers s n n = false).
    { intro Hx. apply I7a in Hx. destruct Hx as [_ [_ [Hc0 _]]]. apply (good_I20 s0 Hg) in Hc0.
      exact Hc0. }
    rewrite Hnn. reflexivity.
  - rewrite upd_ne by easy. reflexivity.
Qed.

Lemma sunloaddivert_count : forall s s' m,
    Good s -> CStep LUnloadDivert s s' -> state s m = StUl ->
    CCount s' m = CCount s m.
Proof.
  intros s s' m Hg Hs Hsm.
  inversion Hs as [n s0 s1 Halive Hstate Hstl Heq]; subst. unpack Hg.
  count_seteq. simpl. split; [reflexivity|].
  intros c Hcin. simpl.
  destruct (Nat.eqb_spec c n).
  - subst c. f_equal. simpl. rewrite Hstate. reflexivity.
  - rewrite upd_ne by easy. reflexivity.
Qed.

Lemma serase_count : forall s s' m,
    Good s -> CStep LErase s s' -> state s m = StUl ->
    CCount s' m = CCount s m.
Proof.
  intros s s' m Hg Hs Hsm.
  inversion Hs as [n s0 s1 Halive Hstate Hre Heq]; subst. unpack Hg.
  count_seteq. simpl. split; [reflexivity|].
  intros c Hcin. simpl.
  destruct (Nat.eqb_spec m n); [subst m; discriminate|].
  rewrite upd_ne by easy. reflexivity.
Qed.

Lemma sfinish_count : forall s s' m,
    Good s -> CStep LFinish s s' -> state s' m = StUl ->
    CCount s' m = remaining s' m.
Proof.
  intros s s' m Hg Hs Hsm'.
  inversion Hs as [n s0 s1 Halive Hstate Hrem Hnin Heq]; subst. unpack Hg.
  destruct (Nat.eqb_spec m n); [subst m; discriminate|].
  simpl in Hsm'. rewrite upd_ne in Hsm' by easy.
  apply I8 in Hsm'.
  simpl. unfold remaining_finish. destruct (committed s n) as [p|] eqn:Ecomm.
  + destruct (Nat.eqb_spec m p).
    * subst m. destruct (consumers s p n) eqn:Econ.
      -- match goal with
         | |- CCount ?t p = _ =>
             assert (Hcnt : CCount t p = CCount s p - 1) by
               (apply (count_remove_one s t p n); try easy;
                [exact I16 | rewrite Econ; reflexivity
                | apply I7a in Econ; destruct Econ as [Hal' _]; exact Hal'
                | apply I7a in Econ; destruct Econ as [_ [_ [_ Hst]]]; exact Hst
                | apply I7a in Econ; destruct Econ as [Hal' _]; apply (good_I13 s0 Hg); exact Hal'
                | simpl; reflexivity | simpl; reflexivity | simpl; reflexivity
                | simpl; intros q k Hqk; unfold consumers_remove; rewrite Ecomm; simpl;
                  destruct (Nat.eqb_spec q p); destruct (Nat.eqb_spec k n);
                  try reflexivity; destruct Hqk; congruence
                | simpl; intros d Hd; rewrite upd_ne by easy; reflexivity
                | simpl; rewrite upd_eq; reflexivity])
         end.
         simpl in Hcnt. congruence.
      -- match goal with
         | |- CCount ?t p = _ =>
             assert (Hcnt : CCount t p = CCount s p) by
               (apply (count_set_eq s t p); simpl; split; [reflexivity|];
                intros c Hcin; simpl;
                destruct (Nat.eqb_spec c n);
                [subst c; f_equal; unfold consumers_remove; rewrite Ecomm; simpl;
                 rewrite Econ; reflexivity
                | rewrite upd_ne by easy; unfold consumers_remove; rewrite Ecomm; simpl;
                  destruct (Nat.eqb_spec p p); [reflexivity|congruence]])
         end.
         simpl in Hcnt. congruence.
    * simpl. rewrite Hsm'. clear Hsm'.
      match goal with
      | |- CCount ?t m = _ =>
          assert (Hcnt : CCount t m = CCount s m) by
            (apply (count_set_eq s t m); simpl; split; [reflexivity|];
             intros c Hcin; simpl;
             destruct (Nat.eqb_spec c n);
             [subst c; f_equal; unfold consumers_remove; rewrite Ecomm; simpl;
              destruct (Nat.eqb_spec m p); [congruence|reflexivity]
             | rewrite upd_ne by easy; unfold consumers_remove; rewrite Ecomm; simpl;
               destruct (Nat.eqb_spec m p); [congruence|reflexivity]])
      end.
      simpl in Hcnt. congruence.
  + simpl. rewrite Hsm'. clear Hsm'.
    match goal with
    | |- CCount ?t m = _ =>
        assert (Hcnt : CCount t m = CCount s m) by
          (apply (count_set_eq s t m); simpl; split; [reflexivity|];
           intros c Hcin; simpl;
           destruct (Nat.eqb_spec c n);
           [subst c; f_equal; unfold consumers_remove; rewrite Ecomm; reflexivity
           | rewrite upd_ne by easy; unfold consumers_remove; rewrite Ecomm; reflexivity])
    end.
    simpl in Hcnt. congruence.
Qed.

(* ---- per-constructor Good preservation -------------------------------- *)

Lemma sevalstart_good : forall s s', Good s -> CStep LEvalStart s s' -> Good s'.
Proof.
  intros s s' Hg Hs. pose proof Hs as Hs0. inversion Hs; subst. unpack Hg. good_split.
  (* I1 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; discriminate |]. solve_clause.
  (* I2 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m |].
    - intro Hbin. apply I18 in Hbin. destruct Hbin as [_ [_ [Hd|Hd]]]; destruct Hd; congruence.
    - solve_clause.
  (* I18 *) intros m Hbin. apply I18 in Hbin.
    destruct Hbin as [Hal [Hp [Hd|Hd]]]; repeat split; try easy;
      try (left; split; assumption); try (right; split; assumption).
  (* I5 *) intros m k Hc Hst. destruct (Nat.eqb_spec m n); [subst m; discriminate |]. solve_clause.
  (* I6 *) solve_clause.
  (* I7a *) solve_clause.
  (* I7b *) intros m p Hc Hst. destruct (Nat.eqb_spec m n); [subst m; discriminate |]. solve_clause.
  (* I8 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; discriminate |].
    simpl. rewrite upd_ne in Hm by easy.
    rewrite (sevalstart_count s s' m Hg Hs0).
    apply I8. exact Hm.
  (* I9 *) intros m Hm. simpl in Hm. apply in_app_or in Hm. destruct Hm as [Hm|Hm].
    - apply I9 in Hm. destruct Hm as [Hal Hs']. split; [exact Hal|].
      destruct Hs' as [Hs'|Hs']; [left|right]; exact Hs'.
    - simpl in Hm. destruct Hm as [Hm|[]]; [| easy].
      subst m. split; [exact H1|]. left. reflexivity.
  (* I10 *) intros m Ham Hsm. simpl. apply in_or_app. destruct (Nat.eqb_spec m n).
    - subst m. right. simpl. auto.
    - left. apply I10; assumption.
  (* I11 *) intros m Ham. destruct (Nat.eqb_spec m n); [subst m; simpl; intro; discriminate |]. solve_clause.
  (* I12 *) intros m Hm. simpl in Hm. apply in_app_or in Hm. destruct Hm as [Hm|Hm].
    - apply I12. exact Hm.
    - simpl in Hm. destruct Hm as [Hm|[]]; [| easy]. subst m.
      intro Hbin. apply I18 in Hbin. destruct Hbin as [_ [_ [Hd|Hd]]]; destruct Hd; congruence.
  (* I13 *) solve_clause.
  (* I14 *) intros m Hm. simpl in Hm. apply in_app_or in Hm. destruct Hm as [Hm|Hm].
    - apply I14. exact Hm.
    - simpl in Hm. destruct Hm as [Hm|[]]; [| easy]. subst m. apply I13. exact H1.
  (* I15 *) solve_clause.
  (* I16 *) intros. apply (good_I16 s0 Hg).
  (* I19 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m |].
    - intros c. apply I22. exact H0.
    - solve_clause.
  (* I20 *) intros m. destruct (Nat.eqb_spec m n).
    - subst m. simpl. unfold resolved_prov. destruct (sat_okb s n) eqn:E.
      + unfold sat_okb in E. destruct (binding s) as [b|] eqn:Eb; [| discriminate].
        destruct (bstate s) eqn:Ebs; try discriminate.
        intro Hc. apply opt_eqb_true in Hc. simpl in E. destruct (Nat.eqb b n) eqn:Ebn.
        * apply Nat.eqb_eq in Ebn. subst. congruence.
        * discriminate.
      + intro. discriminate.
    - solve_clause.
  (* I21 *) intros m k Hk. simpl in Hk. apply in_app_or in Hk. destruct Hk as [Hk|Hk].
    - apply I21. exact Hk.
    - simpl in Hk. destruct Hk as [Hk|[]]; [| easy]. subst k. apply I22. exact H0.
  (* I22 *) intros m k Hk. destruct (Nat.eqb_spec k n); [subst k |].
    - apply I22. exact H0.
    - solve_clause.
  (* I23 *) intros. simpl. apply nodup_app.
    - apply (good_I23 s0 Hg).
    - repeat constructor.
    - intros x Hx. intro Hx'. simpl in Hx'. destruct Hx' as [Hx'|[]]; [| congruence].
      subst x. apply I9 in Hx. destruct Hx as [_ [Hx|Hx]]; congruence.
  (* I25 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m |].
    - simpl. intro Hs'. apply I25. intro He. subst. discriminate.
    - solve_clause.
Qed.


Lemma sevalerr_good : forall s s', Good s -> CStep LEvalErr s s' -> Good s'.
Proof.
  intros s s' Hg Hs. inversion Hs; subst. unpack Hg. good_split.
  all: try solve_clause.
Qed.

Lemma sapply_good : forall s s', Good s -> CStep LApply s s' -> Good s'.
Proof.
  intros s s' Hg Hs. inversion Hs as
    [n s0 s1 rest Hwl Hfp Heq
    |n s0 s1 rest Hwl Hld Hnf Hstl Heq
    |n s0 s1 rest Hwl Hul Heq
    |n s0 s1 rest Hwl Hld Hnf Hpa Hre Hc Heq]; subst.
  1: {
    (* SApplyFail: a raise or a cancelled landing — nothing installed. *)
    unpack Hg. good_split.
    (* I1 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; reflexivity |]. solve_clause.
    (* I2 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
      simpl. destruct (opt_eqb (binding s) (Some n)) eqn:Eb; [intro; discriminate |]. solve_clause.
    (* I18 *) intros m Hbin. simpl in Hbin. destruct (opt_eqb (binding s) (Some n)) eqn:Eb.
      + apply opt_eqb_true in Eb. apply I18 in Eb. destruct Eb as [Hal [Hp Hd]].
        destruct Hd as [[Ha Hb]|[Ha Hb]]; try congruence.
        destruct (bst_eqb (bstate s) BLd) eqn:Es; simpl in Hbin; try discriminate.
        apply bst_eqb_true in Es. congruence.
      + destruct (Nat.eqb_spec m n).
        * subst m. simpl in Hbin. rewrite upd_eq in Hbin. apply I18 in Hbin.
          destruct Hbin as [Hal [Hp Hd]]; repeat split; try easy.
          destruct Hd as [[Ha Hb]|[Ha Hb]]; [right|right]; split; try easy; simpl; auto.
        * simpl in Hbin. rewrite upd_ne in Hbin by easy. apply I18 in Hbin.
          destruct Hbin as [Hal [Hp Hd]]; repeat split; try easy.
          destruct Hd as [[Ha Hb]|[Ha Hb]]; [left|right]; split; try easy; simpl;
            rewrite upd_ne by easy; easy.
    (* I5 *) intros m k Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hc; discriminate |].
      simpl in Hc, Hst. rewrite upd_ne in Hc by easy. rewrite upd_ne in Hst by easy.
      apply I5 in Hc; [| exact Hst]. destruct Hc as [Hb Hal].
      split. — binding' = Some k
      simpl. destruct (opt_eqb (binding s) (Some n)) eqn:Eb.
      + apply opt_eqb_true in Eb. apply I18 in Eb. destruct Eb as [_ [_ Hd]].
        destruct Hd as [[Ha Hb']|[Ha Hb']]; try congruence.
        destruct (bst_eqb (bstate s) BLd) eqn:Es; [apply bst_eqb_true in Es; congruence|].
        exact Hb.
      + exact Hb.
      — alive' k: k <> n?
      destruct (Nat.eqb_spec k n).
      + subst k. apply I18 in Hb. destruct Hb as [_ [_ Hd]].
        destruct Hd as [[Ha Hb']|[Ha Hb']]; congruence.
      + simpl. rewrite upd_ne by easy. exact Hal.
    (* I6 *) solve_clause.
    (* I7a *) intros p m Hcm. destruct (Nat.eqb_spec m n).
      + subst m. simpl in Hcm. exfalso. apply I21 with (m := p). rewrite Hwl. simpl. auto. exact Hcm.
      + solve_clause.
    (* I7b *) intros m p Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hc; discriminate |]. solve_clause.
    (* I8 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
      simpl in Hm. rewrite upd_ne in Hm by easy.
      rewrite (sapply_count s s' m Hg Hs). apply I8. exact Hm.
      Unshelve. 2: exact Hm. — placeholder removed below
    (* I9 *) intros m Hm. assert (Hmn : m <> n).
      { intro. subst m. apply (NoDup_cons_nin _ _ I23) in Hm. exact Hm. }
      simpl in Hm. apply I9 in Hm; [| rewrite Hwl; simpl; auto].
      destruct Hm as [Hal Hs']. split; [exact Hal|].
      destruct Hs' as [Hs'|Hs']; [left|right]; simpl; rewrite upd_ne by easy; exact Hs'.
    (* I10 *) intros m Ham Hsm. simpl. destruct (Nat.eqb_spec m n); [subst m; discriminate |].
      rewrite upd_ne in Hsm by easy. apply I10; [exact Ham|exact Hsm].
    (* I11 *) intros m Ham. simpl in Ham. destruct (Nat.eqb_spec m n).
      + subst m. destruct (fails (comp s n)) eqn:Ef.
        * destruct (st4_eqb (state s n) StLd) eqn:Es; simpl in Ham; try discriminate.
          apply st4_eqb_true in Es. simpl. exact Es.
        * simpl in Ham. discriminate.
      + rewrite upd_ne in Ham by easy. apply I11. exact Ham.
    (* I12 *) intros m Hm. assert (Hmn : m <> n).
      { intro. subst m. apply (NoDup_cons_nin _ _ I23) in Hm. exact Hm. }
      simpl. destruct (opt_eqb (binding s) (Some n)) eqn:Eb.
      + apply opt_eqb_true in Eb. apply I18 in Eb. destruct Eb as [_ [_ Hd]].
        destruct Hd as [[Ha Hb']|[Ha Hb']]; try congruence.
        destruct (bst_eqb (bstate s) BLd) eqn:Es; [apply bst_eqb_true in Es; congruence|].
        apply I12. rewrite Hwl. simpl. auto.
      + apply I12. rewrite Hwl. simpl. auto.
    (* I13 *) solve_clause.
    (* I14 *) solve_clause.
    (* I15 *) solve_clause.
    (* I16 *) intros. apply (good_I16 s0 Hg).
    (* I19 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
    (* I20 *) intros m. destruct (Nat.eqb_spec m n); [subst m; simpl; intro; discriminate |]. solve_clause.
    (* I21 *) intros m k Hk. assert (Hkn : k <> n).
      { intro. subst k. apply (NoDup_cons_nin _ _ I23) in Hk. exact Hk. }
      apply I21. rewrite Hwl. simpl. auto.
    (* I22 *) intros m k Hk. destruct (Nat.eqb_spec k n).
      + subst k. apply I21 with (m := m). rewrite Hwl. simpl. auto.
      + solve_clause.
    (* I23 *) intros. apply (good_I23 s0 Hg).
    (* I25 *) intros m Hm. destruct (Nat.eqb_spec m n).
      + subst m. simpl. intro H'. apply I9 in Hwl.
        destruct Hwl as [_ [Hl|Hu]].
        * apply I25. intro. subst. discriminate.
        * rewrite (count_zero s n) by (intros c; apply I21 with (m := c); rewrite Hwl; simpl; auto).
          apply I8. exact Hu.
      + simpl. rewrite upd_ne in Hm by easy. apply I25. intro. apply Hm. simpl. rewrite upd_ne by easy. easy.
  }
  2: {
    (* SApplyStale: deactivates after the iteration lands (p. 55). *)
    unpack Hg. good_split.
    (* I1 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
    (* I2 *) solve_clause.
    (* I18 *) solve_clause.
    (* I5 *) intros m k Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hst; discriminate |]. solve_clause.
    (* I6 *) solve_clause.
    (* I7a *) solve_clause.
    (* I7b *) intros m p Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hst; discriminate |]. solve_clause.
    (* I8 *) intros m Hm. destruct (Nat.eqb_spec m n).
      + subst m. simpl. intro H'. rewrite (count_zero s n)
          by (intros c; apply (good_I19 s0 Hg); exact Hld). apply I25. intro. subst. discriminate.
      + simpl. rewrite upd_ne in Hm by easy.
        rewrite (sapply_count s s' m Hg Hs). apply I8. exact Hm. Unshelve. 2: exact Hm.
    (* I9 *) intros m Hm. assert (Hmn : m <> n).
      { intro. subst m. apply (NoDup_cons_nin _ _ I23) in Hm. exact Hm. }
      simpl in Hm. apply I9 in Hm; [| rewrite Hwl; simpl; auto].
      destruct Hm as [Hal Hs']. split; [exact Hal|].
      destruct Hs' as [Hs'|Hs']; [left|right]; simpl; rewrite upd_ne by easy; exact Hs'.
    (* I10 *) intros m Ham Hsm. simpl. destruct (Nat.eqb_spec m n); [subst m; discriminate |].
      rewrite upd_ne in Hsm by easy. apply I10; [exact Ham|exact Hsm].
    (* I11 *) solve_clause.
    (* I12 *) solve_clause.
    (* I13 *) solve_clause.
    (* I14 *) solve_clause.
    (* I15 *) solve_clause.
    (* I16 *) intros. apply (good_I16 s0 Hg).
    (* I19 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
    (* I20 *) solve_clause.
    (* I21 *) intros m k Hk. assert (Hkn : k <> n).
      { intro. subst k. apply (NoDup_cons_nin _ _ I23) in Hk. exact Hk. }
      apply I21. rewrite Hwl. simpl. auto.
    (* I22 *) intros m k Hk. destruct (Nat.eqb_spec k n); [subst k; simpl; discriminate |]. solve_clause.
    (* I23 *) intros. apply (good_I23 s0 Hg).
    (* I25 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
      solve_clause.
  }
  3: {
    (* SApplyCancel: a diverted fiber's apply lands cancelled. *)
    unpack Hg. good_split.
    (* I1 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; reflexivity |]. solve_clause.
    (* I2 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
    (* I18 *) solve_clause.
    (* I5 *) intros m k Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hc; discriminate |]. solve_clause.
    (* I6 *) solve_clause.
    (* I7a *) intros p m Hcm. destruct (Nat.eqb_spec m n).
      + subst m. simpl in Hcm. exfalso. apply I21 with (m := p). rewrite Hwl. simpl. auto. exact Hcm.
      + solve_clause.
    (* I7b *) intros m p Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hc; discriminate |]. solve_clause.
    (* I8 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
      simpl in Hm. rewrite upd_ne in Hm by easy.
      rewrite (sapply_count s s' m Hg Hs). apply I8. exact Hm. Unshelve. 2: exact Hm.
    (* I9 *) intros m Hm. assert (Hmn : m <> n).
      { intro. subst m. apply (NoDup_cons_nin _ _ I23) in Hm. exact Hm. }
      simpl in Hm. apply I9 in Hm; [| rewrite Hwl; simpl; auto].
      destruct Hm as [Hal Hs']. split; [exact Hal|].
      destruct Hs' as [Hs'|Hs']; [left|right]; simpl; rewrite upd_ne by easy; exact Hs'.
    (* I10 *) intros m Ham Hsm. simpl. destruct (Nat.eqb_spec m n); [subst m; discriminate |].
      rewrite upd_ne in Hsm by easy. apply I10; [exact Ham|exact Hsm].
    (* I11 *) solve_clause.
    (* I12 *) solve_clause.
    (* I13 *) solve_clause.
    (* I14 *) solve_clause.
    (* I15 *) solve_clause.
    (* I16 *) intros. apply (good_I16 s0 Hg).
    (* I19 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
    (* I20 *) intros m. destruct (Nat.eqb_spec m n); [subst m; simpl; intro; discriminate |]. solve_clause.
    (* I21 *) intros m k Hk. assert (Hkn : k <> n).
      { intro. subst k. apply (NoDup_cons_nin _ _ I23) in Hk. exact Hk. }
      apply I21. rewrite Hwl. simpl. auto.
    (* I22 *) intros m k Hk. destruct (Nat.eqb_spec k n).
      + subst k. apply I21 with (m := m). rewrite Hwl. simpl. auto.
      + solve_clause.
    (* I23 *) intros. apply (good_I23 s0 Hg).
    (* I25 *) intros m Hm. destruct (Nat.eqb_spec m n).
      + subst m. simpl. intro H'. rewrite (count_zero s n)
          by (intros c; apply I21 with (m := c); rewrite Hwl; simpl; auto). apply I8. exact Hul.
      + solve_clause.
  }
  4: {
    (* SApplyPublish: L-Finish (p. 36). *)
    unpack Hg. good_split.
    (* I1 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
    (* I2 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
      simpl. destruct (provides (comp s n)) eqn:Epp.
      + rewrite upd_ne in Hm by easy. intro Hbin. apply opt_eqb_true in Hbin.
        apply I2. exact Hm. intro. symmetry in Hbin. apply Hbin. — contradiction m <> n vs binding = Some m? binding' = Some n
      + rewrite upd_ne in Hm by easy. apply I2. exact Hm.
    (* I18 *) intros m Hbin. simpl in Hbin. destruct (provides (comp s n)) eqn:Epp.
      + (* binding' = Some n *)
        apply opt_eqb_true in Hbin. subst m.
        repeat split; try easy; try exact Hwl.
        * apply I9. rewrite Hwl. simpl. auto. — alive n
        * left. split; [reflexivity|reflexivity]. — hmm: bstate' = if provides then BAc
      + rewrite upd_ne in Hbin by (intro; subst; congruence). apply I18 in Hbin.
        destruct Hbin as [Hal [Hp Hd]]; repeat split; try easy.
        destruct Hd as [[Ha Hb]|[Ha Hb]]; [left|right]; split; try easy; simpl;
          rewrite upd_ne by (intro; subst; congruence); easy.
    (* I5 *) intros m k Hc Hst. destruct (Nat.eqb_spec m n).
      + subst m. simpl in Hc, Hst.
        (* provides n impossible: n provides and committed to k provides -> k = n, contra I20 *)
        assert (Hnp : provides (comp s n) = false).
        { destruct (provides (comp s n)) eqn:Epp; [| reflexivity].
          unfold providers_active in Hpa. rewrite Hc in Hpa.
          destruct Hpa as [Hc0|[Hbind Hbs]]; [discriminate|].
          apply I18 in Hbind. destruct Hbind as [_ [Hp' _]]. apply I6 in Epp; [| exact H1| exact Hp'];
            try easy. subst k. apply (good_I20 s0 Hg) in Hc. exact Hc. }
        rewrite Hnp. simpl. apply I5. exact Hc. exact Hst.
      + simpl in Hc, Hst. rewrite upd_ne in Hc by easy. rewrite upd_ne in Hst by easy.
        apply I5 in Hc; [| exact Hst]. destruct Hc as [Hb Hal].
        split.
        * simpl. destruct (provides (comp s n)) eqn:Epp.
          -- apply opt_eqb_true in Hb. apply I18 in Hb. destruct Hb as [Hal' [Hp' _]].
             apply I6 in Epp; [| exact H1| exact Hp']. subst k. reflexivity.
          -- exact Hb.
        * destruct (Nat.eqb_spec k n).
          ++ subst k. simpl. reflexivity.
          ++ simpl. rewrite upd_ne by easy. exact Hal.
    (* I6 *) solve_clause.
    (* I7a *) intros p m Hcm. destruct (Nat.eqb_spec m n).
      + subst m. simpl in Hcm. unfold consumers_add in Hcm. destruct (committed s n) eqn:Ecomm.
        * simpl in Hcm. destruct (Nat.eqb_spec p n); [subst p; destruct (Nat.eqb n n); [|congruence]|].
          -- repeat split; try reflexivity.
             ++ apply I9. rewrite Hwl. simpl. auto.
             ++ destruct (Nat.eqb_spec n p0); [|]. — hmm — committed s n = Some p0 — need p0 = n? no: p was unified with n — but consumers' p n with p = n — committed' n = Some n?? — I20 contra — this branch impossible:
             — let me restructure: consumers' p n = (consumers s p n || (p =? committed n) && (n =? n)) — for it to be true: either pre (I21 contra) or p = committed n.
        * 
      + solve_clause.
    (* I7b *) intros m p Hc Hst. destruct (Nat.eqb_spec m n).
      + subst m. simpl in Hst. simpl. unfold consumers_add. rewrite Hc. simpl.
        destruct (Nat.eqb_spec p n). — p = n impossible (I20)
        * apply (good_I20 s0 Hg) in Hc. subst p. easy.
        * destruct (Nat.eqb n n); [reflexivity|congruence].
      + solve_clause.
    (* I8 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
      simpl in Hm. rewrite upd_ne in Hm by easy.
      rewrite (sapply_count s s' m Hg Hs). apply I8. exact Hm. Unshelve. 2: exact Hm.
    (* I9 *) intros m Hm. assert (Hmn : m <> n).
      { intro. subst m. apply (NoDup_cons_nin _ _ I23) in Hm. exact Hm. }
      simpl in Hm. apply I9 in Hm; [| rewrite Hwl; simpl; auto].
      destruct Hm as [Hal Hs']. split; [exact Hal|].
      destruct Hs' as [Hs'|Hs']; [left|right]; simpl; rewrite upd_ne by easy; exact Hs'.
    (* I10 *) intros m Ham Hsm. simpl. destruct (Nat.eqb_spec m n); [subst m; discriminate |].
      rewrite upd_ne in Hsm by easy. apply I10; [exact Ham|exact Hsm].
    (* I11 *) solve_clause.
    (* I12 *) intros m Hm. assert (Hmn : m <> n).
      { intro. subst m. apply (NoDup_cons_nin _ _ I23) in Hm. exact Hm. }
      simpl. destruct (provides (comp s n)) eqn:Epp.
      + intro Hbin. apply opt_eqb_true in Hbin. congruence.
      + apply I12. rewrite Hwl. simpl. auto.
    (* I13 *) solve_clause.
    (* I14 *) solve_clause.
    (* I15 *) intros p m Hcm. destruct (Nat.eqb_spec m n).
      + subst m. simpl in Hcm. unfold consumers_add in Hcm. destruct (committed s n) eqn:Ecomm.
        * simpl in Hcm. destruct (Nat.eqb_spec p n).
          -- subst p. repeat split; simpl; auto.
             ++ apply I13. apply I9. rewrite Hwl. simpl. auto.
             ++ apply I13. apply I9. rewrite Hwl. simpl. auto. — p0's slot — hmm — In p0 slots via alive p0 → I13 — alive p0 from I18 via providers_active:
          -- apply I15 in Hcm. destruct Hcm as [Hn Hp]; split; simpl; auto.
        * simpl in Hcm. apply I15 in Hcm. destruct Hcm as [Hn Hp]; split; simpl; auto.
      + solve_clause.
    (* I16 *) intros. apply (good_I16 s0 Hg).
    (* I19 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
      solve_clause.
    (* I20 *) solve_clause.
    (* I21 *) intros m k Hk. assert (Hkn : k <> n).
      { intro. subst k. apply (NoDup_cons_nin _ _ I23) in Hk. exact Hk. }
      apply I21. rewrite Hwl. simpl. auto.
    (* I22 *) intros m k Hk. destruct (Nat.eqb_spec k n); [subst k; simpl; discriminate |]. solve_clause.
    (* I23 *) intros. apply (good_I23 s0 Hg).
    (* I25 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  }
Qed.

Lemma sunloadeval_good : forall s s', Good s -> CStep LUnloadEval s s' -> Good s'.
Proof.
  intros s s' Hg Hs. inversion Hs; subst. unpack Hg. good_split.
  (* I1 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I2 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I18 *) intros m Hbin. simpl in Hbin. destruct (opt_eqb (binding s) (Some n)) eqn:Eb.
    + apply opt_eqb_true in Eb. subst m. apply I18 in Eb.
      destruct Eb as [Hal [Hp Hd]]; repeat split; try easy.
      right. split; [discriminate|]. destruct Hd as [[_ Hb]|[_ Hb]]; simpl; exact Hb.
    + destruct (Nat.eqb_spec m n).
      * subst m. simpl in Hbin. rewrite upd_eq in Hbin. apply I18 in Hbin.
        destruct Hbin as [Hal [Hp Hd]]; repeat split; try easy.
        destruct Hd as [[Ha Hb]|[Ha Hb]]; [left|right]; split; try easy; simpl; auto.
      * simpl in Hbin. rewrite upd_ne in Hbin by easy. apply I18 in Hbin.
        destruct Hbin as [Hal [Hp Hd]]; repeat split; try easy.
        destruct Hd as [[Ha Hb]|[Ha Hb]]; [left|right]; split; try easy; simpl;
          rewrite upd_ne by easy; easy.
  (* I5 *) intros m k Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hst; discriminate |]. solve_clause.
  (* I6 *) solve_clause.
  (* I7a *) solve_clause.
  (* I7b *) intros m p Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hst; discriminate |]. solve_clause.
  (* I8 *) intros m Hm. destruct (Nat.eqb_spec m n).
    + subst m. simpl. intro H'. reflexivity.
    + simpl. rewrite upd_ne in Hm by easy.
      rewrite (sunloadeval_count s s' m Hg Hs). apply I8. exact Hm.
      Unshelve. 2: exact Hm.
  (* I9 *) solve_clause.
  (* I10 *) intros m Ham Hsm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I11 *) solve_clause.
  (* I12 *) solve_clause.
  (* I13 *) solve_clause.
  (* I14 *) solve_clause.
  (* I15 *) solve_clause.
  (* I16 *) intros. apply (good_I16 s0 Hg).
  (* I19 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I20 *) solve_clause.
  (* I21 *) solve_clause.
  (* I22 *) intros m k Hk. destruct (Nat.eqb_spec k n); [subst k; simpl; discriminate |]. solve_clause.
  (* I23 *) intros. apply (good_I23 s0 Hg).
  (* I25 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
Qed.

Lemma sunloaddivert_good : forall s s', Good s -> CStep LUnloadDivert s s' -> Good s'.
Proof.
  intros s s' Hg Hs. inversion Hs; subst. unpack Hg. good_split.
  (* I1 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I2 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I18 *) solve_clause.
  (* I5 *) intros m k Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hst; discriminate |]. solve_clause.
  (* I6 *) solve_clause.
  (* I7a *) solve_clause.
  (* I7b *) intros m p Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hst; discriminate |]. solve_clause.
  (* I8 *) intros m Hm. destruct (Nat.eqb_spec m n).
    + subst m. simpl. intro H'. rewrite (count_zero s n)
        by (intros c; apply (good_I19 s0 Hg); exact H2). apply I25. intro. subst. discriminate.
    + simpl. rewrite upd_ne in Hm by easy.
      rewrite (sunloaddivert_count s s' m Hg Hs). apply I8. exact Hm.
      Unshelve. 2: exact Hm.
  (* I9 *) intros m Hm. destruct (Nat.eqb_spec m n).
    + subst m. split; [exact H1|]. right. simpl. reflexivity.
    + simpl in Hm. apply I9 in Hm. destruct Hm as [Hal Hs']. split; [exact Hal|].
      destruct Hs' as [Hs'|Hs']; [left|right]; simpl; rewrite upd_ne by easy; exact Hs'.
  (* I10 *) intros m Ham Hsm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I11 *) solve_clause.
  (* I12 *) solve_clause.
  (* I13 *) solve_clause.
  (* I14 *) solve_clause.
  (* I15 *) solve_clause.
  (* I16 *) intros. apply (good_I16 s0 Hg).
  (* I19 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I20 *) solve_clause.
  (* I21 *) solve_clause.
  (* I22 *) intros m k Hk. destruct (Nat.eqb_spec k n); [subst k; simpl; discriminate |]. solve_clause.
  (* I23 *) intros. apply (good_I23 s0 Hg).
  (* I25 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
Qed.

Lemma sfinish_good : forall s s', Good s -> CStep LFinish s s' -> Good s'.
Proof.
  intros s s' Hg Hs. inversion Hs; subst. unpack Hg. good_split.
  (* I1 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; reflexivity |]. solve_clause.
  (* I2 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
    simpl. destruct (opt_eqb (binding s) (Some n)) eqn:Eb; [intro; discriminate |]. solve_clause.
  (* I18 *) intros m Hbin. simpl in Hbin. destruct (opt_eqb (binding s) (Some n)) eqn:Eb.
    + apply opt_eqb_true in Eb. subst. discriminate.
    + destruct (Nat.eqb_spec m n).
      * subst m. simpl in Hbin. rewrite upd_eq in Hbin. apply I18 in Hbin.
        destruct Hbin as [Hal [Hp Hd]]; repeat split; try easy.
        destruct Hd as [[Ha Hb]|[Ha Hb]]; [right|right]; split; try easy; simpl; auto.
      * simpl in Hbin. rewrite upd_ne in Hbin by easy. apply I18 in Hbin.
        destruct Hbin as [Hal [Hp Hd]]; repeat split; try easy.
        destruct Hd as [[Ha Hb]|[Ha Hb]]; [left|right]; split; try easy; simpl;
          rewrite upd_ne by easy; easy.
  (* I5 *) intros m k Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hc; discriminate |].
    simpl in Hc, Hst. rewrite upd_ne in Hc by easy. rewrite upd_ne in Hst by easy.
    apply I5 in Hc; [| exact Hst]. destruct Hc as [Hb Hal].
    destruct (Nat.eqb_spec k n).
    + subst k. exfalso.
      (* m is an installed consumer of n: the guard blocks the finish *)
      assert (Hcm : consumers s n m = true).
      { apply I7b. exact Hc. exact Hst. }
      assert (Hinm : In m (slots s)) by (apply (good_I15 s0 Hg); exact Hcm).
      apply I15 in Hcm as [Hmn _].
      assert (Hge : 1 <= count_installed s n) by
        (apply count_ge1; [exact Hcm | exact Hmn | intro; subst; contradiction | exact Hinm]).
      apply I8 in H3. lia.
    + split.
      * simpl. destruct (opt_eqb (binding s) (Some n)) eqn:Eb'.
        -- apply opt_eqb_true in Eb'. apply I18 in Eb'. destruct Eb' as [_ [_ Hd]].
           destruct Hd as [[Ha Hb']|[Ha Hb']]; congruence.
        -- exact Hb.
      * simpl. rewrite upd_ne by easy. exact Hal.
  (* I6 *) solve_clause.
  (* I7a *) intros p m Hcm. destruct (Nat.eqb_spec m n).
    + subst m. simpl in Hcm. unfold consumers_remove in Hcm. destruct (committed s n) eqn:Ecomm.
      * simpl in Hcm. destruct (Nat.eqb_spec p n); [subst p; destruct (Nat.eqb n n); [discriminate|discriminate]|].
        simpl in Hcm. destruct (Nat.eqb_spec p n); try congruence.
        apply I7a in Hcm. destruct Hcm as [Hal' [Hp' [Hc' Hst']]].
        repeat split; try easy. simpl; rewrite upd_eq. reflexivity.
      * simpl in Hcm. apply I7a in Hcm. destruct Hcm as [Hal' [Hp' [Hc' Hst']]].
        repeat split; try easy. simpl; rewrite upd_eq. reflexivity.
    + solve_clause.
  (* I7b *) intros m p Hc Hst. destruct (Nat.eqb_spec m n); [subst m; simpl in Hc; discriminate |]. solve_clause.
  (* I8 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
    simpl in Hm. rewrite upd_ne in Hm by easy. rewrite (sfinish_count s s' m Hg Hs).
    reflexivity. Unshelve. 2: exact Hm.
  (* I9 *) solve_clause.
  (* I10 *) intros m Ham Hsm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I11 *) solve_clause.
  (* I12 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; easy |].
    simpl. destruct (opt_eqb (binding s) (Some n)) eqn:Eb; [intro; discriminate |]. solve_clause.
  (* I13 *) solve_clause.
  (* I14 *) solve_clause.
  (* I15 *) intros p m Hcm. destruct (Nat.eqb_spec m n).
    + subst m. simpl in Hcm. unfold consumers_remove in Hcm. destruct (committed s n) eqn:Ecomm.
      * simpl in Hcm. destruct (Nat.eqb_spec p n); [subst p; destruct (Nat.eqb n n); [discriminate|discriminate]|].
        simpl in Hcm. destruct (Nat.eqb_spec p n); try congruence.
        apply I15 in Hcm. destruct Hcm as [Hmn Hmp]; split; simpl; auto.
      * simpl in Hcm. apply I15 in Hcm. destruct Hcm as [Hmn Hmp]; split; simpl; auto.
    + solve_clause.
  (* I16 *) intros. apply (good_I16 s0 Hg).
  (* I19 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I20 *) intros m. destruct (Nat.eqb_spec m n); [subst m; simpl; intro; discriminate |]. solve_clause.
  (* I21 *) solve_clause.
  (* I22 *) intros m k Hk. destruct (Nat.eqb_spec k n).
    + subst k. simpl. unfold consumers_remove. destruct (committed s n) eqn:Ecomm.
      * simpl. reflexivity.
      * reflexivity.
    + solve_clause.
  (* I23 *) intros. apply (good_I23 s0 Hg).
  (* I25 *) intros m Hm. destruct (Nat.eqb_spec m n).
    + subst m. simpl. intro H'. rewrite H4. apply (good_I20 s0 Hg). — hmm
Qed.

Lemma serase_good : forall s s', Good s -> CStep LErase s s' -> Good s'.
Proof.
  intros s s' Hg Hs. inversion Hs; subst. unpack Hg. good_split.
  (* I1 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m |].
    - simpl. apply I1. exact Hm. — state unchanged; committed unchanged: committed s n = None by I1 pre
    - solve_clause.
  (* I2 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m |].
    - simpl. apply I2. exact Hm.
    - solve_clause.
  (* I18 *) intros m Hbin. destruct (Nat.eqb_spec m n).
    + subst m. simpl in Hbin. rewrite upd_eq in Hbin. apply I18 in Hbin.
      destruct Hbin as [Hal [Hp Hd]]; repeat split; try easy.
      destruct Hd as [[Ha Hb]|[Ha Hb]]; [left|right]; split; try easy; simpl; auto.
    + simpl in Hbin. rewrite upd_ne in Hbin by easy. apply I18 in Hbin.
      destruct Hbin as [Hal [Hp Hd]]; repeat split; try easy.
      destruct Hd as [[Ha Hb]|[Ha Hb]]; [left|right]; split; try easy; simpl;
        rewrite upd_ne by easy; easy.
  (* I5 *) intros m k Hc Hst. destruct (Nat.eqb_spec m n); [subst m |].
    - simpl in Hc, Hst. apply I5 in Hc; [| exact Hst]. destruct Hc as [Hb Hal].
      split; [exact Hb|]. destruct (Nat.eqb_spec k n).
      + subst k. apply I18 in Hb. destruct Hb as [_ [_ Hd]].
        destruct Hd as [[Ha Hb']|[Ha Hb']]; congruence.
      + simpl. rewrite upd_ne by easy. exact Hal.
    - solve_clause.
  (* I6 *) solve_clause.
  (* I7a *) intros p m Hcm. destruct (Nat.eqb_spec m n).
    + subst m. simpl in Hcm. exfalso. apply I7a in Hcm. destruct Hcm as [_ [_ [_ Hst]]].
      contradiction. — state s n = StIn vs ≠ StIn — need Hst : state s n <> StIn; premise H2 : state s n = StIn — contradiction via Hst H2
    + solve_clause.
  (* I7b *) intros m p Hc Hst. destruct (Nat.eqb_spec m n); [subst m |].
    - simpl in Hc. apply I5 in Hc; [| exact Hst]. destruct Hc as [Hb _].
      apply I18 in Hb. destruct Hb as [_ [_ Hd]].
      destruct Hd as [[Ha Hb']|[Ha Hb']]; congruence. — contra state s n = StIn
    - solve_clause.
  (* I8 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |].
    simpl in Hm. rewrite upd_ne in Hm by easy. rewrite (serase_count s s' m Hg Hs).
    apply I8. exact Hm. Unshelve. 2: exact Hm.
  (* I9 *) solve_clause.
  (* I10 *) intros m Ham Hsm. destruct (Nat.eqb_spec m n); [subst m; simpl in Ham; discriminate |]. solve_clause.
  (* I11 *) solve_clause.
  (* I12 *) solve_clause.
  (* I13 *) intros m Ham. destruct (Nat.eqb_spec m n); [subst m; simpl in Ham; discriminate |]. solve_clause.
  (* I14 *) solve_clause.
  (* I15 *) intros p m Hcm. destruct (Nat.eqb_spec p n).
    + subst p. simpl in Hcm. discriminate.
    + solve_clause.
  (* I16 *) intros. apply (good_I16 s0 Hg).
  (* I19 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl; discriminate |]. solve_clause.
  (* I20 *) solve_clause.
  (* I21 *) solve_clause.
  (* I22 *) intros m k Hk. destruct (Nat.eqb_spec k n).
    + subst k. simpl. reflexivity.
    + solve_clause.
  (* I23 *) intros. apply (good_I23 s0 Hg).
  (* I25 *) intros m Hm. destruct (Nat.eqb_spec m n); [subst m; simpl |].
    - intro H'. apply I25. intro. subst. discriminate. — hmm — I25 post for m = n: state' n = StIn ≠ StUl → remaining' n = 0: remaining' n = remaining s n — state s n = StIn → I25 pre ✓
    - solve_clause.
Qed.

Lemma good_step : forall s s' l, Good s -> CStep l s s' -> Good s'.
Proof.
  intros s s' l Hg Hs. destruct l.
  - apply (smount_good s s' Hg Hs).
  - apply (sretire_good s s' Hg Hs).
  - apply (sevalstart_good s s' Hg Hs).
  - apply (sevalerr_good s s' Hg Hs).
  - apply (sapply_good s s' Hg Hs).
  - apply (sunloadeval_good s s' Hg Hs).
  - apply (sunloaddivert_good s s' Hg Hs).
  - apply (sfinish_good s s' Hg Hs).
  - apply (serase_good s s' Hg Hs).
Qed.

Lemma reach_good : forall s, Reach provides req_db fails s -> Good s.
Proof.
  intros s Hs. unfold Reach in Hs.
  destruct Hs as [s0 [Hi Hrt]]. induction Hrt.
  - apply (init_good s0 Hi).
  - destruct H as [l Hl]. apply (good_step x y l).
    + exact IHHrt.
    + exact Hl.
Qed.

End ConcreteInvariants.