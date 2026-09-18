----------------------------- MODULE MC ------------------------------------
(* The model-checking instance: three fiber names and the component pool
   shared with the C++ conformance universe (tests/model_test.cpp and
   proof/conformance/conformance_test.cpp): P provider, C required
   consumer, O optional consumer, S self-provider, B broken (raising)
   consumer. The C++ universe drives two slots; the third widens the
   interleaving net (replacement cascades through three slots).
   S is confined by the single-source premise of O-Insert (p. 34)
   exactly as the conformance universe confines it to one slot. L starts
   unavailable (AvailInit) and flips through AvailabilityFlip. *)

EXTENDS Integers, FiniteSets, Sequences

Slot == {1, 2, 3}
Comp == {"P", "C", "O", "S", "B", "L"}

(* Declaring the variables here binds the instances' variables to these
   by name - the standard TLC instantiation pattern. *)
VARIABLES alive, comp, state, committed, err, afailed, reactivate,
          remaining, consumers, binding, bstate, wl

(* L is the lazy provider: provides db, publishes unavailable (AvailInit),
   and flips through AvailabilityFlip (runtime::signal_availability). *)
A == INSTANCE ArayaMachine WITH Slot <- Slot, Comp <- Comp,
       Provides <- [c \in Comp |-> c \in {"P", "S", "L"}],
       ReqDb <- [c \in Comp |-> c \in {"C", "B"}],
       FAILS <- [c \in Comp |-> c = "B"],
       AvailInit <- [c \in Comp |-> c \in {"P", "S"}]

R == INSTANCE Refine WITH Slot <- Slot, Comp <- Comp,
       Provides <- [c \in Comp |-> c \in {"P", "S", "L"}],
       ReqDb <- [c \in Comp |-> c \in {"C", "B"}],
       FAILS <- [c \in Comp |-> c = "B"],
       AvailInit <- [c \in Comp |-> c \in {"P", "S"}]

Spec == A!Spec
TypeOK == A!TypeOK
Refinement == R!Refinement

(* Progress (Theorem 73): the guard always releases and the system
   quiesces. Checked as a liveness property under weak fairness on the
   internal actions. *)
Quiescence == <>A!Quiescent

=============================================================================
