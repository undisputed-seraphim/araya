----------------------------- MODULE PaperRules -----------------------------
(* The paper's calculus, Section 4.2, pp. 34-37, at the abstraction level
   Araya refines against. Every rule carries its page number; the theorems
   of Section 4.3 (Temporal composability Thm 68, Spatial composability
   Thm 70, Progress Thm 73, Confluence Thm 80) are taken as proven for
   THIS system and transferred to the engine by the refinement in
   Refine.tla.
   State per fiber n (the paper's gamma, with F_gamma the registry):
     palive[n]   - n in dom(F_gamma)
     pcomp[n]    - the component n was inserted with (d_n, p_n, e_n;
                   Definition 48; immutable by Lemma 59(5))
     pstate[n]   - theta_n in {Inactive, Reloading, Active, Unloading}
     pview[n]    - omega_n, the committed view (Definition 49)
     pretired[n] - tau_n, the retirement flag (O-Retire, p. 34)
     pok[n]      - the Failure-extension outcome (p. 56): FALSE withholds
                   re-entry (L-Begin reads as requiring an error-free fiber)
     pbound      - sigma_gamma(db): the one shared realm (Section 4.1) with
                   the single key "example.db" read at it. p. 33 eq. (46)
                   is the union over ACTIVE fiber tables; with one key this
                   is the provider name or 0. *)

EXTENDS Integers, FiniteSets, Sequences

CONSTANTS Slot,      \* fiber names (the paper's N)
          Comp,      \* component identifiers (the paper's C_Gamma)
          Provides,  \* Provides[c]: c declares the provision of db
          ReqDb      \* ReqDb[c]: c declares db as a required read

VARIABLES palive, pcomp, pstate, pview, pretired, pok, pbound

varsAbs == <<palive, pcomp, pstate, pview, pretired, pok, pbound>>

TypeOKAbs ==
  /\ palive \in [Slot -> BOOLEAN]
  /\ pcomp \in [Slot -> Comp]
  /\ pstate \in [Slot -> {"in", "ld", "ac", "ul"}]
  /\ pview \in [Slot -> (Slot \cup {0})]
  /\ pretired \in [Slot -> BOOLEAN]
  /\ pok \in [Slot -> BOOLEAN]
  /\ pbound \in (Slot \cup {0})

InitAbs ==
  /\ palive = [n \in Slot |-> FALSE]
  /\ pcomp \in [Slot -> Comp]
  /\ pstate = [n \in Slot |-> "in"]
  /\ pview = [n \in Slot |-> 0]
  /\ pretired = [n \in Slot |-> FALSE]
  /\ pok = [n \in Slot |-> TRUE]
  /\ pbound = 0

(* Whether the declared keys are satisfied (Section 3.2.2): a required key
   must be bound by an ACTIVE fiber other than n itself. The self-exclusion
   makes the paper's precedence-acyclicity assumption effective: the
   degenerate n prec n (p. 49) never resolves. *)
SatAbs(n) == ~ReqDb[pcomp[n]] \/ (pbound # 0 /\ pbound # n)

(* Definition 53, p. 35 eq. (48): the target view. Bottom (-1) when n
   ought not to be running at all: retired (tau_n), or unsatisfied;
   otherwise the provider of db - 0 is the EMPTY view (an optional
   declaration binding nothing yet), which is distinct from bottom: a
   fiber whose target turns to bottom mid-transition diverts even when it
   committed nothing (L-Divert's target != omega holds: 0 # -1). *)
TargetAbs(n) ==
  IF pretired[n] \/ ~SatAbs(n) THEN -1 ELSE pbound

(* Definition 54, p. 36 eq. (50): relied upon - some other INSTALLED
   fiber's committed view names n (the installed_m premise). A loading
   consumer is not yet relied: the provider may withdraw while the
   consumer's transition is in flight - Araya's consumer then reads its
   committed-view snapshot, and its completion-time re-check (the stale
   branch of ApplyComplete) diverts it, which is the paper's L-Divert.
   This is the guard of L-Unload. *)
ReliedAbs(n) ==
  \E m \in Slot : palive[m] /\ m # n /\ pview[m] = n /\ pstate[m] = "ac"

(* ---- orchestration rules (gamma => delta), Section 4.2.1, p. 34 ------- *)

OInsert(n, c) ==
  /\ ~palive[n]
  (* p. 34: forall m in dom(F_gamma). p cap p_m = empty - the
     single-source discipline. Read at the one key: at most one alive
     provider of db at a time. *)
  /\ Provides[c] =>
       ~ \E m \in Slot : palive[m] /\ Provides[pcomp[m]]
  /\ palive' = [palive EXCEPT ![n] = TRUE]
  /\ pcomp' = [pcomp EXCEPT ![n] = c]
  /\ pstate' = [pstate EXCEPT ![n] = "in"]
  /\ pview' = [pview EXCEPT ![n] = 0]
  /\ pretired' = [pretired EXCEPT ![n] = FALSE]
  /\ pok' = [pok EXCEPT ![n] = TRUE]
  /\ UNCHANGED pbound

ORetire(n) ==
  /\ palive[n] /\ ~pretired[n]
  /\ pretired' = [pretired EXCEPT ![n] = TRUE]
  /\ UNCHANGED <<palive, pcomp, pstate, pview, pok, pbound>>

ORemove(n) ==
  (* p. 34: tau_n = top, theta_n = Inactive, sigma_n = empty, no children.
     The no-children premise (forall m. pi_m != n) is vacuous in this
     universe, which has no instantiation (Definition 52, v2). *)
  /\ palive[n] /\ pretired[n] /\ pstate[n] = "in"
  /\ palive' = [palive EXCEPT ![n] = FALSE]
  /\ UNCHANGED <<pcomp, pstate, pview, pretired, pok, pbound>>

(* ---- lifecycle rules (gamma --> delta), Section 4.2.2, pp. 36-37 ------ *)

LBgin(n) ==
  (* p. 36: theta_n = Inactive, omega = target_n(gamma) != bottom. *)
  /\ palive[n] /\ pstate[n] = "in" /\ pok[n] /\ ~pretired[n] /\ SatAbs(n)
  /\ pstate' = [pstate EXCEPT ![n] = "ld"]
  /\ pview' = [pview EXCEPT ![n] = TargetAbs(n)]
  /\ UNCHANGED <<palive, pcomp, pretired, pok, pbound>>

(* L-Iter (p. 36) is a stutter at our granularity: Araya's apply runs as a
   single coroutine body, the unit iterator (Definition 56, Lemma 57
   confinement) with no intermediate iteration boundary. *)

LFinish(n) ==
  (* p. 36: the last iteration lands. Under the Asynchrony extension the
     target is re-checked at completion (pp. 55-56). *)
  /\ palive[n] /\ pstate[n] = "ld"
  /\ pview[n] = TargetAbs(n) /\ SatAbs(n) /\ ~pretired[n]
  /\ pstate' = [pstate EXCEPT ![n] = "ac"]
  /\ pbound' = (IF Provides[pcomp[n]] THEN n ELSE pbound)
  /\ UNCHANGED <<palive, pcomp, pview, pretired, pok>>

LFail(n) ==
  (* The Failure extension, p. 56: a raise exits Reloading by the route of
     an aborting L-Divert, arrives Inactive having installed nothing
     (Corollary 69), and writes the outcome. The outcome withholds
     re-entry; a retry is a revision (the reinserted fiber starts without
     an outcome). *)
  /\ palive[n] /\ pstate[n] = "ld"
  /\ pstate' = [pstate EXCEPT ![n] = "in"]
  /\ pok' = [pok EXCEPT ![n] = FALSE]
  /\ pview' = [pview EXCEPT ![n] = 0]
  /\ UNCHANGED <<palive, pcomp, pretired, pbound>>

LDivert(n) ==
  (* p. 37: theta_n = Reloading, target != omega. Araya takes only the
     landing alternative (Asynchrony, p. 55: a map in flight runs to
     completion whether or not it is still wanted). *)
  /\ palive[n] /\ pstate[n] = "ld" /\ pview[n] # TargetAbs(n)
  /\ pstate' = [pstate EXCEPT ![n] = "ul"]
  /\ UNCHANGED <<palive, pcomp, pview, pretired, pok, pbound>>

LLeave(n) ==
  (* p. 37: theta_n = Active, target != omega. The marking removes n's
     table from sigma_gamma (p. 37: "once L-Leave has marked n, its table
     leaves sigma_gamma"). *)
  /\ palive[n] /\ pstate[n] = "ac" /\ pview[n] # TargetAbs(n)
  /\ pstate' = [pstate EXCEPT ![n] = "ul"]
  /\ pbound' = (IF pbound = n THEN 0 ELSE pbound)
  /\ UNCHANGED <<palive, pcomp, pview, pretired, pok>>

LUnload(n) ==
  (* p. 37: theta_n = Unloading, not relied_n(gamma): the guard defers the
     withdrawal until every consumer that committed to n has gone
     (Theorem 70; Theorem 73 shows the guard always releases). Applies
     the accumulator and discards the committed view. *)
  /\ palive[n] /\ pstate[n] = "ul" /\ ~ReliedAbs(n)
  /\ pstate' = [pstate EXCEPT ![n] = "in"]
  /\ pview' = [pview EXCEPT ![n] = 0]
  /\ UNCHANGED <<palive, pcomp, pretired, pok, pbound>>

NextAbs ==
  \/ \E n \in Slot : ORetire(n) \/ LBgin(n) \/ LFinish(n) \/ LFail(n)
                     \/ LDivert(n) \/ LLeave(n) \/ LUnload(n) \/ ORemove(n)
  \/ \E n \in Slot, c \in Comp : OInsert(n, c)

SpecAbs == InitAbs /\ [][NextAbs]_varsAbs

(* Definition 53, p. 35 eq. (49): quiet - every fiber settled at its target
   view, no transition in progress. The Failure extension admits a failed
   fiber whatever its target view (p. 56). *)
QuietAbs ==
  \A n \in Slot : palive[n] =>
    pstate[n] \in {"in", "ac"}
    /\ (pstate[n] = "ac" => SatAbs(n) /\ pview[n] = TargetAbs(n) /\ pview[n] # 0)
    /\ (pstate[n] = "in" => (~pok[n] \/ TargetAbs(n) = -1))

=============================================================================
