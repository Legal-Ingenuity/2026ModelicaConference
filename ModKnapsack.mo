/*
 * ModKnapsack.mo — OpenModelica interface to the quantum 0/1 knapsack solver (knapsack/knapsack.c).
 *
 * Modules:
 *   TestCase          record    one knapsack instance: N, Costs, Values, default MaxCost and SolverPath
 *   TestCases         package   the library of instances (one constant per case)
 *   ModCallKnapsack   function  external "C" wrapper around callKnapsack()
 *   KnapsackRun       model     select a case, set MaxCost / SolverPath, simulate with stopTime = 0
 *
 * Build: cmake (README.md) puts libknapsack.a and libqiskit.a in Resources/Library/linux64.
 * Run:  loadFile("ModKnapsack.mo"); simulate(ModKnapsack.KnapsackRun, stopTime=0);
 *       or, in Modelon Impact, simulate ModKnapsack.KnapsackRun with start = stop = 0.
 *       Another case: model Run extends ModKnapsack.KnapsackRun(tc=ModKnapsack.TestCases.TC3_N5, MaxCost=4); end Run;
 *
 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-10
 */
package ModKnapsack "Quantum 0/1 Knapsack Solver via the Qiskit C API"

  // TestCase — one knapsack instance (costs and values are integers stored as Real).
  record TestCase "One 0/1 knapsack instance"
    parameter Integer N "Number of items";
    parameter Real Costs[N] "Cost of each item (integer-valued)";
    parameter Real Values[N] "Value of each item (integer-valued)";
    parameter Real MaxCost "Default budget W for this case";
    parameter Integer SolverPath "Default solver path for this case: 0 sim, 1 Aer, 2 IBM QPU";
    parameter String Name "Short label";
  end TestCase;

  // TestCases — the library. TC1 = loose budget (W = 50% of total cost), TC2 = tight budget (25%),
  // TC3 = unique optimum, instance 1 of each at N = 2..7; Golden_N4 = the install-gate case;
  // HW_N2 = the instance run on IBM Marrakesh.
  package TestCases "Library of knapsack instances"
    constant TestCase Golden_N4 = TestCase(N=4, Costs={2,3,4,5},       Values={3,4,5,7},       MaxCost=6, SolverPath=0, Name="Golden N=4 (install gate)");
    constant TestCase HW_N2     = TestCase(N=2, Costs={1,2},           Values={2,3},           MaxCost=3, SolverPath=0, Name="Marrakesh N=2 (hardware run)");
    constant TestCase TC1_N2 = TestCase(N=2, Costs={1,1},             Values={2,2},             MaxCost=1, SolverPath=0, Name="TC1 N=2");
    constant TestCase TC1_N3 = TestCase(N=3, Costs={1,2,2},           Values={2,1,1},           MaxCost=2, SolverPath=0, Name="TC1 N=3");
    constant TestCase TC1_N4 = TestCase(N=4, Costs={3,1,1,2},         Values={3,3,2,1},         MaxCost=3, SolverPath=0, Name="TC1 N=4");
    constant TestCase TC1_N5 = TestCase(N=5, Costs={2,3,1,3,2},       Values={2,1,3,1,3},       MaxCost=5, SolverPath=0, Name="TC1 N=5");
    constant TestCase TC1_N6 = TestCase(N=6, Costs={2,1,1,1,3,1},     Values={3,3,1,3,3,2},     MaxCost=4, SolverPath=0, Name="TC1 N=6");
    constant TestCase TC1_N7 = TestCase(N=7, Costs={2,1,1,3,3,2,1},   Values={1,1,3,1,2,3,2},   MaxCost=6, SolverPath=0, Name="TC1 N=7 (~5 min)");
    constant TestCase TC2_N2 = TestCase(N=2, Costs={1,2},             Values={2,2},             MaxCost=1, SolverPath=0, Name="TC2 N=2");
    constant TestCase TC2_N3 = TestCase(N=3, Costs={2,1,3},           Values={2,2,3},           MaxCost=1, SolverPath=0, Name="TC2 N=3");
    constant TestCase TC2_N4 = TestCase(N=4, Costs={2,3,1,1},         Values={2,1,1,2},         MaxCost=1, SolverPath=0, Name="TC2 N=4");
    constant TestCase TC2_N5 = TestCase(N=5, Costs={3,2,2,3,2},       Values={2,1,3,3,2},       MaxCost=3, SolverPath=0, Name="TC2 N=5");
    constant TestCase TC2_N6 = TestCase(N=6, Costs={2,2,3,1,3,3},     Values={1,2,2,2,3,1},     MaxCost=3, SolverPath=0, Name="TC2 N=6");
    constant TestCase TC2_N7 = TestCase(N=7, Costs={3,2,1,1,3,2,2},   Values={3,2,1,2,3,2,2},   MaxCost=3, SolverPath=0, Name="TC2 N=7 (~5 min)");
    constant TestCase TC3_N2 = TestCase(N=2, Costs={3,1},             Values={1,3},             MaxCost=2, SolverPath=0, Name="TC3 N=2");
    constant TestCase TC3_N3 = TestCase(N=3, Costs={1,3,1},           Values={3,3,2},           MaxCost=2, SolverPath=0, Name="TC3 N=3");
    constant TestCase TC3_N4 = TestCase(N=4, Costs={3,1,2,2},         Values={3,1,2,3},         MaxCost=4, SolverPath=0, Name="TC3 N=4");
    constant TestCase TC3_N5 = TestCase(N=5, Costs={2,3,2,1,1},       Values={2,2,3,2,3},       MaxCost=4, SolverPath=0, Name="TC3 N=5");
    constant TestCase TC3_N6 = TestCase(N=6, Costs={1,1,1,1,1,3},     Values={3,3,1,3,2,2},     MaxCost=4, SolverPath=0, Name="TC3 N=6");
    constant TestCase TC3_N7 = TestCase(N=7, Costs={1,1,1,1,2,3,2},   Values={3,2,2,1,1,3,2},   MaxCost=5, SolverPath=0, Name="TC3 N=7 (~5 min)");
  end TestCases;

  // ModCallKnapsack — the external C call: void, every output by pointer (OpenModelica passes scalar
  // outputs by value if the C return value is also mapped; the all-pointer form avoids that).
  //   Kind: 0 = exact distribution of the final circuit (path 0); 1 = verification shots (paths 1/2).
  //   PSubset[s+1] = P(subset with bitmask s), bit i set = item i chosen.
  // Linked from Resources/Library/linux64 (libknapsack.a + libqiskit.a, built by CMake), not
  // #included: the same annotation works in OpenModelica and in Modelon Impact. Impure: the
  // search is randomized and writes files.
  impure function ModCallKnapsack "Calls callKnapsack() in knapsack/knapsack.c"
    input  Integer Npatents "Number of items (1..Nmax, Nmax = 7)";
    input  Real    Values[Npatents];
    input  Real    Costs[Npatents];
    input  Real    MaxCost "Budget W";
    input  Integer QDrawDetail "Circuit drawing 0..5";
    input  Integer SolverPath "0 sim, 1 Aer, 2 IBM QPU";
    input  Integer NStates "= 2^Npatents (size of PSubset)";
    output Integer PatentIndices[Npatents] "1 = item (i-1) selected";
    output Integer ExStatus "0 found, 1 verification run, -1 no feasible, -2 invalid, -3 too large, -6 timed out";
    output Real    PSubset[NStates] "Probability of each subset (sums to 1)";
    output Integer OracleCalls "Path 0: oracle calls in the search; 0 otherwise";
    output Integer Shots "Paths 1/2: shots measured; 0 otherwise";
    output Integer Kind "0 exact final-circuit distribution (path 0), 1 verification shots (paths 1/2)";
    external "C" callKnapsack(Npatents, Values, Costs, MaxCost, PatentIndices, ExStatus, QDrawDetail, SolverPath,
                              NStates, PSubset, OracleCalls, Shots, Kind)
      annotation(
        Library = {"knapsack", "qiskit", "m"},
        LibraryDirectory = "modelica://ModKnapsack/Resources/Library");
  end ModCallKnapsack;

  // KnapsackRun — pick a case, optionally override MaxCost / SolverPath, simulate with stopTime = 0.
  // Npatents, Values, Costs come from the selected case; MaxCost, QDrawDetail, SolverPath are inputs.
  model KnapsackRun "Run the quantum knapsack solver on a library test case"
    import Modelica.Utilities.Streams;
    parameter TestCase tc = TestCases.Golden_N4 "Test case (pick from the list)"
      annotation(choices(
        choice=ModKnapsack.TestCases.Golden_N4 "Golden N=4 (install gate)",
        choice=ModKnapsack.TestCases.HW_N2 "Marrakesh N=2 (hardware run)",
        choice=ModKnapsack.TestCases.TC1_N2 "TC1 N=2", choice=ModKnapsack.TestCases.TC1_N3 "TC1 N=3",
        choice=ModKnapsack.TestCases.TC1_N4 "TC1 N=4", choice=ModKnapsack.TestCases.TC1_N5 "TC1 N=5",
        choice=ModKnapsack.TestCases.TC1_N6 "TC1 N=6", choice=ModKnapsack.TestCases.TC1_N7 "TC1 N=7",
        choice=ModKnapsack.TestCases.TC2_N2 "TC2 N=2", choice=ModKnapsack.TestCases.TC2_N3 "TC2 N=3",
        choice=ModKnapsack.TestCases.TC2_N4 "TC2 N=4", choice=ModKnapsack.TestCases.TC2_N5 "TC2 N=5",
        choice=ModKnapsack.TestCases.TC2_N6 "TC2 N=6", choice=ModKnapsack.TestCases.TC2_N7 "TC2 N=7",
        choice=ModKnapsack.TestCases.TC3_N2 "TC3 N=2", choice=ModKnapsack.TestCases.TC3_N3 "TC3 N=3",
        choice=ModKnapsack.TestCases.TC3_N4 "TC3 N=4", choice=ModKnapsack.TestCases.TC3_N5 "TC3 N=5",
        choice=ModKnapsack.TestCases.TC3_N6 "TC3 N=6", choice=ModKnapsack.TestCases.TC3_N7 "TC3 N=7"));

    // ---- the problem, as the C code sees it (the six values the C entry point takes) ----
    final parameter Integer Npatents = tc.N "Number of items (from the test case)";
    final parameter Real    Values[Npatents] = tc.Values "Item values (from the test case)";
    final parameter Real    Costs[Npatents]  = tc.Costs  "Item costs (from the test case)";
    parameter Real    MaxCost     = tc.MaxCost    "Budget W (default from the test case)";
    parameter Integer QDrawDetail = 0             "Circuit drawing 0..5 (0 none, 1 text, 2 light, 3 expanded, 4/5 OpenQASM export)";
    parameter Integer SolverPath  = tc.SolverPath "0 pure-C statevector sim, 1 Aer noise model, 2 IBM QPU (1/2 wait for a results file)";
    final parameter Integer NStates = integer(2^Npatents) "Number of subsets";
    final parameter Real    Chance  = 1/NStates "Probability of the optimum by a blind draw";
    final parameter Integer K = min(8, NStates) "Rows in the printed distribution";

    // ---- the standard result block: the same variables for every solver path ----
    discrete Integer PatentIndices[Npatents](each start=0) "1 = item (i-1) selected";
    discrete Integer ExStatus(start=0);
    discrete Real    ActualCost(start=0)  "Cost of the returned subset (computed here)";
    discrete Real    ActualValue(start=0) "Value of the returned subset (computed here)";
    discrete Real    PSubset[NStates](each start=0) "P(subset s), s = index-1 as a bitmask";
    discrete Real    PSuccess(start=0) "P of the returned subset";
    discrete Integer OracleCalls(start=0);
    discrete Integer Shots(start=0);
    discrete Integer Kind(start=0) "0 exact final-circuit distribution (path 0), 1 verification shots (paths 1/2)";
    discrete Integer BestMask(start=0) "Bitmask of the returned subset";
    discrete Integer top[K](each start=0) "Indices of the K most probable subsets";

  protected
    function maskOf "bitmask of a 0/1 indicator vector (bit i-1 = item i)"
      input Integer x[:]; output Integer m;
    algorithm
      m := 0;
      for i in 1:size(x,1) loop
        if x[i] == 1 then m := m + integer(2^(i-1)); end if;
      end for;
    end maskOf;

    function itemsOf "the items of a bitmask as a string, e.g. {0, 2}"
      input Integer mask; input Integer N; output String str;
    protected Boolean first = true;
    algorithm
      str := "{";
      for i in 0:N-1 loop
        if mod(div(mask, integer(2^i)), 2) == 1 then
          str := str + (if first then "" else ", ") + String(i); first := false;
        end if;
      end for;
      str := str + "}";
    end itemsOf;

    function sumOf "sum of w over the items of a bitmask"
      input Integer mask; input Real w[:]; output Real total;
    algorithm
      total := 0;
      for i in 1:size(w,1) loop
        if mod(div(mask, integer(2^(i-1))), 2) == 1 then total := total + w[i]; end if;
      end for;
    end sumOf;

    function topK "indices (1-based) of the K largest entries of P, descending"
      input Real P[:]; input Integer K; output Integer idx[K];
    protected Boolean used[size(P,1)] = fill(false, size(P,1)); Integer best;
    algorithm
      for k in 1:K loop
        best := 0;
        for s in 1:size(P,1) loop
          if not used[s] and (best == 0 or P[s] > P[best]) then best := s; end if;
        end for;
        idx[k] := best; used[best] := true;
      end for;
    end topK;

    function listOf "the entries of an Integer vector, comma separated, e.g. 1, 0, 1, 0"
      input Integer x[:]; output String str;
    algorithm
      str := "";
      for i in 1:size(x,1) loop
        str := str + (if i > 1 then ", " else "") + String(x[i]);
      end for;
    end listOf;

    function statusText
      input Integer ExStatus; output String str;
    algorithm
      str := if ExStatus == 0 then "optimal found"
        else if ExStatus == 1 then "verification run (Aer/QPU, threshold from the classical check)"
        else if ExStatus == -1 then "no feasible subset (MaxCost too small)"
        else if ExStatus == -3 then "too large for the quantum solver (N > Nmax = 7)"
        else if ExStatus == -6 then "solver path timed out (no results file)"
        else "input / results error";
    end statusText;

    function pathText
      input Integer SolverPath; output String str;
    algorithm
      str := if SolverPath == 0 then "pure-C statevector" else if SolverPath == 1 then "Aer noise model" else "IBM QPU";
    end pathText;

  algorithm
    when initial() then
      (PatentIndices, ExStatus, PSubset, OracleCalls, Shots, Kind) :=
          ModCallKnapsack(Npatents, Values, Costs, MaxCost, QDrawDetail, SolverPath, NStates);
      ActualCost := 0; ActualValue := 0;
      for i in 1:Npatents loop
        if PatentIndices[i] == 1 then
          ActualCost  := ActualCost  + Costs[i];
          ActualValue := ActualValue + Values[i];
        end if;
      end for;
      BestMask := maskOf(PatentIndices);
      PSuccess := PSubset[BestMask + 1];

      // Each line whole: Modelica.Utilities.Streams.print ends every call with a newline.
      Streams.print("");
      Streams.print("=== ModKnapsack: " + tc.Name + " ===");
      Streams.print("  N = " + String(Npatents) + "    W = " + String(MaxCost) + "    SolverPath = " + String(SolverPath)
            + " (" + pathText(SolverPath) + ")    QDrawDetail = " + String(QDrawDetail));
      Streams.print("");
      Streams.print("  Item   Cost   Value   Selected");
      for i in 1:Npatents loop
        Streams.print(String(i-1, minimumLength=6, leftJustified=false) + String(integer(Costs[i]), minimumLength=7, leftJustified=false)
              + String(integer(Values[i]), minimumLength=8, leftJustified=false) + "   " + (if PatentIndices[i] == 1 then "yes" else "."));
      end for;
      Streams.print("");
      Streams.print("  Result: ExStatus " + String(ExStatus) + " (" + statusText(ExStatus) + ")"
            + (if ExStatus == 0 or ExStatus == 1 then
                 "   subset " + itemsOf(BestMask, Npatents) + "   cost " + String(integer(ActualCost)) + " of " + String(MaxCost)
                 + "   value " + String(integer(ActualValue))
               else ""));
      if Kind == 0 then
        Streams.print("  Search: " + String(OracleCalls) + " oracle calls   (Kind 0 = exact distribution of the final circuit)");
      else
        Streams.print("  Shots:  " + String(Shots) + "   (Kind 1 = verification run on " + pathText(SolverPath) + ")");
      end if;
      // the three lines the install gate reads
      Streams.print("  ExStatus      = " + String(ExStatus));
      Streams.print("  PatentIndices = [" + listOf(PatentIndices) + "]");
      Streams.print("  ActualCost    = " + String(ActualCost) + "   ActualValue = " + String(ActualValue));
      if ExStatus == 0 or ExStatus == 1 then
        Streams.print("");
        Streams.print("  Subset distribution (top " + String(K) + " of " + String(NStates) + ", P descending)");
        Streams.print("  mask   cost  value  fits        P   items");
        top := topK(PSubset, K);
        for r in 1:K loop
          Streams.print(String(top[r]-1, minimumLength=6, leftJustified=false) + String(integer(sumOf(top[r]-1, Costs)), minimumLength=7, leftJustified=false) + String(integer(sumOf(top[r]-1, Values)), minimumLength=7, leftJustified=false)
                + "   " + (if sumOf(top[r]-1, Costs) <= MaxCost then "yes" else "no ") + String(PSubset[top[r]], significantDigits=4, minimumLength=9, leftJustified=false)
                + "   " + itemsOf(top[r]-1, Npatents));
        end for;
        Streams.print("  chance 1/" + String(NStates) + " = " + String(Chance, significantDigits=4) + "   P(returned subset) = " + String(PSuccess, significantDigits=4));
      end if;
    end when;
  end KnapsackRun;

  annotation(uses(Modelica(version = "4.1.0")));
end ModKnapsack;
