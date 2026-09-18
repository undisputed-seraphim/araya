----------------------------- MODULE Refine --------------------------------
(* The refinement: every step of ArayaMachine is a rule of the paper's
   calculus (PaperRules, Section 4.2, pp. 34-37) or a stutter, under the
   abstraction function below. Assuming the paper's metatheory (Preserva-
   tion, Theorems 68/70/73/80, Section 4.3), the refinement transfers it
   to the engine: a violation in Araya would project through alpha into a
   violation of the paper.

   The abstraction contract (proof/README.md):
     observable - fiber lifecycle states, the committed view, the binding
                  map, the retirement flag, the failure outcome;
     stutters   - err bookkeeping, the guard counter, the consumer index,
                  the pending-apply queue, the binding's provider-state
                  encoding.

   Step correspondence (each ArayaMachine action -> paper rule):
     Mount          -> O-Insert   (p. 34)
     RetireFlag     -> O-Retire   (p. 34)
     EvaluateStart  -> L-Begin    (p. 36)
     ApplyComplete  -> L-Finish / L-Fail (p. 56) / L-Divert landing
                       (p. 55) / stutter (cancelled: L-Unload on a
                       vacuous guard, p. 37)
     UnloadEval     -> L-Leave    (p. 37)
     UnloadDivert   -> L-Divert   (p. 37)
     Finish         -> L-Unload   (p. 37)
     Erase          -> O-Remove   (p. 34)
     EvaluateErr    -> stutter *)

EXTENDS ArayaMachine

(* ---- the abstraction function alpha ------------------------------------ *)

ABound == IF bstate = "ac" THEN binding ELSE 0
ARetired == [n \in Slot |-> reactivate[n] = -1]
AOk == [n \in Slot |-> ~afailed[n]]
(* The availability extension reads through a lagged state: an
   unavailable provider (binding = n, bstate = "ld") has landed
   concretely but has not yet published from the paper's point of view,
   so alpha keeps it loading. Its promotion (AvailabilityFlip) is the
   paper's L-Finish; its landing (ApplyComplete's publish branch) is a
   stutter. *)
AState == [n \in Slot |-> IF binding = n /\ bstate = "ld" THEN "ld"
                           ELSE state[n]]

P == INSTANCE PaperRules WITH
      palive <- alive,
      pcomp <- comp,
      pstate <- AState,
      pview <- committed,
      pretired <- ARetired,
      pok <- AOk,
      pbound <- ABound

(* Stuttering: only the alpha-images must hold still. With the
   availability lag (AState) and the binding reading (ABound), the
   concrete encodings state/binding/bstate may move while their images
   do not - e.g. an unavailable provider's landing is a stutter. *)
StutterAlpha ==
  UNCHANGED <<alive, comp, AState, committed, reactivate, afailed,
              ABound>>

(* The paper's spec read through alpha, as a temporal property of the
   concrete behavior: every concrete step either is a paper rule or
   leaves the abstraction untouched. *)
Refinement == P!InitAbs /\ [][P!NextAbs \/ StutterAlpha]_vars

=============================================================================
