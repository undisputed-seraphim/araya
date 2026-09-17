----------------------------- MODULE MC ------------------------------------
(* The model-checking instance: two fiber names and the component pool
   shared with the C++ conformance universe (tests/model_test.cpp and
   proof/conformance/conformance_test.cpp): P provider, C required
   consumer, O optional consumer, S self-provider, B broken (raising)
   consumer. S is confined by the single-source premise of O-Insert
   (p. 34) exactly as the conformance universe confines it to one slot. *)

EXTENDS Integers, FiniteSets, Sequences

Slot == {1, 2}
Comp == {"P", "C", "O", "S", "B"}

(* Declaring the variables here binds the instances' variables to these
   by name - the standard TLC instantiation pattern. *)
VARIABLES alive, comp, state, committed, err, afailed, reactivate,
          remaining, consumers, binding, bstate, wl

A == INSTANCE ArayaMachine WITH Slot <- Slot, Comp <- Comp,
       Provides <- [c \in Comp |-> c \in {"P", "S"}],
       ReqDb <- [c \in Comp |-> c \in {"C", "B"}],
       FAILS <- [c \in Comp |-> c = "B"]

R == INSTANCE Refine WITH Slot <- Slot, Comp <- Comp,
       Provides <- [c \in Comp |-> c \in {"P", "S"}],
       ReqDb <- [c \in Comp |-> c \in {"C", "B"}],
       FAILS <- [c \in Comp |-> c = "B"]

Spec == A!Spec
TypeOK == A!TypeOK
Refinement == R!Refinement

(* Progress (Theorem 73): the guard always releases and the system
   quiesces. Checked as a liveness property under weak fairness on the
   internal actions. *)
Quiescence == <>A!Quiescent

=============================================================================
