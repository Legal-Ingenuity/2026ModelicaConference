# ModKnapsack — an OpenModelica × Qiskit quantum knapsack toolchain

A proof-of-concept integration that drives the IBM Qiskit C API from a Modelica model through the
Modelica external-C interface, and exercises the resulting solver on small 0/1 knapsack instances
drawn from patent-portfolio analysis. The model runs in **OpenModelica** and in **Modelon Impact**.

This repository accompanies the paper *Quantum Algorithms for Patent Portfolio Analysis Using
OpenModelica and IBM Qiskit*, accepted for the 2026 North American Modelica & FMI Conference
(Atlanta, October 12–14, 2026). The version of record is `ModKnapsack_Modelica2026-Paper.pdf` (also
deposited on Zenodo, DOI [10.5281/zenodo.22034029](https://doi.org/10.5281/zenodo.22034029)). The
paper's test cases stop at four items; the code in this repository solves instances of up to seven
items.

## Status: work in progress / proof of concept

The purpose of this project is to demonstrate the **integration** of a quantum software stack
(Qiskit) with an equation-based modeling environment — not to present a new quantum algorithm, a
scientific discovery, or a finished, production-ready framework. It makes **no claim of quantum
advantage**: on every instance tried, the quantum search needs more oracle calls than brute-force
enumeration (the advantage is asymptotic). The toolchain is under active development and its
interfaces may change.

## Contents

- `ModKnapsack.mo` — the Modelica package: a `TestCase` record, a `TestCases` library of
  instances, the external-C function `ModCallKnapsack`, and the model `KnapsackRun` that you
  simulate
- `knapsack/` — the C solver (`knapsack.c`, one file) and its nine self-test programs
- `CMakeLists.txt`, `build_qiskit.sh`, `run_knapsack.mos` — build configuration, the Qiskit C API
  build, and the one-line OpenModelica run script
- `verify_qasm.py`, `noise_study.py`, `plot_noise.py`, `draw_circuit.py`, `hardware_run.py` —
  optional out-of-band Python tools that consume the exported OpenQASM-3 circuit
  (`hardware_run.py` is a demonstration of Qiskit Runtime submission; see *Solver paths* below)
- `ModKnapsack_Installation_Instructions.pdf` — build-from-scratch guide (Linux); see
  *Changes since the installation guide* below
- `Knapsack_Documentation.pdf` — engineering documentation (algorithms, code structure, tests);
  its opening section lists what changed since it was written
- `ALGORITHM_REFERENCES.pdf` — literature map for the algorithms used
- `README.md` — this document (it replaces `README.pdf`, which describes the state before
  Modelon Impact support)

Qiskit itself is **not** bundled; you build its C API locally as described below (validated
against Qiskit commit `61e3ca016`). The buildable project sits at the repository root.
`ModKnapsack.mo` finds its libraries relative to its own location (a `modelica://` URI), so
nothing in it needs editing after a clone; keep `qiskit/` inside the project root.

## How the model calls the solver

`KnapsackRun` selects an instance from the `TestCases` library (twenty instances: the paper's
four-item golden case, the two-item hardware case, and three families TC1 loose budget / TC2 tight
budget / TC3 unique optimum at N = 2 … 7), lets you override the budget `MaxCost` and the
`SolverPath`, and calls the C entry point once at initialisation (`stopTime = 0`):

```c
void callKnapsack(int Npatents, const double *Values, const double *Costs, double MaxCost,
                  int *PatentIndices, int *ExStatus, int QDrawDetail, int SolverPath,
                  int NStates, double *PSubset, int *OracleCalls, int *Shots, int *Kind);
```

Every output is passed by pointer and the function returns `void` (OpenModelica passes scalar
outputs by value if a C return value is also mapped). Outputs:

| Output | Meaning |
|---|---|
| `PatentIndices[i]` | 1 if item *i* is in the returned subset, else 0 |
| `ExStatus` | 0 optimum found · 1 verification run (paths 1/2) · −1 no feasible subset · −2 invalid input · −3 N > 7 · −6 external solver timed out |
| `PSubset[s]` | probability of the subset with bitmask *s* (`NStates = 2^N` entries, sums to 1) |
| `OracleCalls` | path 0: oracle calls spent by the search; 0 otherwise |
| `Shots` | paths 1/2: shots measured; 0 otherwise |
| `Kind` | 0 = exact distribution of the final circuit (path 0), 1 = verification shots (paths 1/2) |

The model computes `ActualCost` and `ActualValue` from `PatentIndices`, prints the instance, the
result, and the most probable subsets (with `Modelica.Utilities.Streams.print`, so the text lands
in the OpenModelica output and in the Modelon Impact simulation log), and compares the returned
subset's probability with the blind-draw chance 1/2^N. Costs and values are small integers
(fractional inputs are rounded). `QDrawDetail` 0–5 selects an optional circuit drawing or
OpenQASM-3 export of one oracle + diffuser iteration.

`ModCallKnapsack` links the solver as a library:

```modelica
external "C" callKnapsack(...)
  annotation(Library = {"knapsack", "qiskit", "m"},
             LibraryDirectory = "modelica://ModKnapsack/Resources/Library");
```

Both tools look in `Resources/Library/linux64/`, where the CMake build puts `libknapsack.a` (the
solver, compiled as C11) and `libqiskit.a` (the Qiskit C API with libpython folded in). Static
libraries, so a simulation needs no `LD_LIBRARY_PATH` and no Python. The package uses the Modelica
Standard Library 4.1.0 (`uses(Modelica(version = "4.1.0"))`).

## Solver paths

- **`SolverPath = 0` — statevector simulation (the default).** A pure-C statevector simulator
  built into `knapsack.c` runs the Grover search (Boyer–Brassard–Høyer–Tapp iteration schedule
  inside a Dürr–Høyer maximum search), best of `GROVER_K = 3` quantum solves, with a classical
  check of the returned subset. `PSubset` is the exact output distribution of the final circuit.
  Up to `KNAPSACK_NMAX = 7` items (21 qubits).
- **`SolverPath = 1` (Aer noise model) and `2` (IBM QPU) — out-of-process verification runs.** The
  solver exports the *verification circuit* (one Grover search at the known optimum value, at the
  Grover-optimal iteration count) as `solverCircuit.qasm` plus a sidecar `solverCircuit.meta`, then
  waits for a results file — `aer_results.json` or `qpu_results.json` in the working directory —
  polling every 2 s for up to 2 h (`KNAPSACK_WAIT_TIMEOUT_S` overrides the limit). **This
  repository does not include the program that runs the circuit and writes that file**; without
  one, paths 1 and 2 end with `ExStatus = −6` after the timeout. A runner must execute the exported
  circuit and write, atomically (write to a temporary name, then rename), a JSON object with these
  keys:

  | key | content |
  |---|---|
  | `request_id` | the integer from `solverCircuit.meta` (must match this request) |
  | `path` | 1 or 2, as in the sidecar |
  | `shots` | number of shots |
  | `counts_by_mask` | array of 2^N integers, index = item bitmask (bit *i* = item *i*), summing to `shots` |
  | `backend`, `job_id` | strings, reported only |

  The sidecar lists `N`, `NQ`, `W`, `T` (threshold), `j` (iterations), `path`, `request_id`, the
  `item_qubits` to measure and the `marked_masks` (the optimal subsets). The solver validates the
  results file against the request, takes the most frequent feasible measured subset as the
  answer, returns the measured distribution (share of shots per subset) as `PSubset`, and flags the
  run with `ExStatus = 1` / `Kind = 1`.

  In Modelon Impact the working directory is the case's own folder on the server, so paths 1 and 2
  are of little use there; keep `SolverPath = 0`.

## Build and test

Full instructions, from a bare Ubuntu machine, are in `ModKnapsack_Installation_Instructions.pdf`
(Rust, OpenModelica, the Qiskit clone at commit `61e3ca016`). From the project root, with
`qiskit/` cloned and checked out there:

```bash
./build_qiskit.sh /usr/local/bin/python3.11   # Qiskit C API -> qiskit/dist/c/ (.so, .a, header)
cmake -S . -B cmake-build-debug && cmake --build cmake-build-debug
for t in gate_test adder_test cmp_test oracle_test diffuser_test solver_test qasm_test draw_test knapsack_test; do
  ./cmake-build-debug/$t > /dev/null || echo "$t FAILED"; done
omc run_knapsack.mos   # golden case: items 0 and 2, cost 6, value 8, ExStatus 0
```

`build_qiskit.sh` replaces the guide's `make c` (it runs it) and also builds the static library
the model links. Its argument is a Python interpreter with a **static, position-independent
libpython** (`libpythonX.Y.a` in its `LIBPL` directory): the Qiskit C API at this commit still
contains pyo3, and linking libpython statically is what keeps Python out of the simulation. In a
Modelon Impact container, `/usr/local/bin/python3.11` is such an interpreter. The CMake build then
puts `libknapsack.a` and `libqiskit.a` into `Resources/Library/linux64/` for the model, beside the
shared library and the self-test programs in `cmake-build-debug/` (which find `libqiskit.so`
through their run path; no `LD_LIBRARY_PATH` needed).

`knapsack_test` exercises `callKnapsack()` itself on seven cases and exits non-zero on any failure;
`solver_test` and `knapsack_test` run full searches (a couple of minutes each, more on a slow
machine). To run another library instance from a script or OMEdit:

```modelica
model Run extends ModKnapsack.KnapsackRun(tc = ModKnapsack.TestCases.TC3_N5, MaxCost = 4); end Run;
```

### Modelon Impact

Build as above in the Impact container's terminal (CMake is available; install Rust locally with
rustup, no root needed), in the project's folder under `local_projects/`. Then, in a workspace that
holds the project:

1. Open `ModKnapsack.KnapsackRun`; pick the test case from the `tc` drop-down and, if wanted,
   `MaxCost` and `SolverPath`.
2. Simulate with start time 0 and stop time 0 (the solve happens at initialisation).
3. The result is in the variables (`ExStatus`, `PatentIndices`, `ActualCost`, `ActualValue`,
   `PSubset`, …) and the printed table in the simulation log.

The FMU carries both static libraries, so nothing else needs to be installed on the server. Any
file the solver writes (`groverStats.txt`, for one) shows up as a
downloadable artifact in the case's simulation log.

### Tested with

| | |
|---|---|
| OpenModelica | 1.27.1, MSL 4.1.0, Ubuntu 22.04, GCC 11.4 |
| Modelon Impact | OCT 1.70.0, MSL 4.1.0 (the Impact container: Ubuntu 22.04, GCC 11.4) |
| CMake | 4.1 |
| Qiskit | commit `61e3ca016`, built with Rust 1.87 (from Qiskit's `rust-toolchain.toml`) against Python 3.11.17 |

Both tools return the same result for the golden case: subset {0, 2}, cost 6, value 8,
`ExStatus = 0`, 59 oracle calls, P(returned subset) = 0.9613.

### Changes since the installation guide

The guide (July 2026, updated October 2026) predates Modelon Impact support:

- Section 5.2: run `./build_qiskit.sh <python>` from the project root instead of `make c` in
  `qiskit/` (the guide's `pip install .` is needed only for the optional Python tools).
- Sections 6.1 and 8: `LD_LIBRARY_PATH` is no longer needed, neither for the self-tests nor for
  `omc run_knapsack.mos`.
- Section 8: `run_knapsack.mos` loads the Modelica Standard Library 4.1.0 first; install it once
  with `installPackage(Modelica, "4.1.0")` in OpenModelica if it is missing.

## Disclaimer

The code and models in this repository are released under the MIT License (see `LICENSE`) and are
provided strictly **"as is"**, without warranty of any kind, express or implied, including but not
limited to the warranties of merchantability, fitness for a particular purpose, and
non-infringement. The author makes no representation regarding the accuracy, completeness, or
suitability of this software for any purpose, and assumes **no liability or accountability** for
any errors or omissions, or for any damages or system failures arising from its use. Use it at
your own risk.

## Use of AI tools

During the preparation of this work the author used Anthropic's Claude (via Claude Code) as a
development and editing assistant. All algorithmic design, code, mathematical formulations and
text were directed, independently reviewed and verified by the author, who retains sole
intellectual property ownership of, and full responsibility for, the final work. The same
statement appears in the header of every source file and in the paper's Acknowledgment.

## Third-party notices

- **Qiskit** (IBM, Apache License 2.0) is a build-time dependency. It is not bundled; you clone it
  and build the C API locally as described above, and `qiskit/` is git-ignored. The static
  `libqiskit.a` the build produces also contains the CPython library of the interpreter passed to
  `build_qiskit.sh` (Python Software Foundation License); it stays local and is git-ignored.
- The `mcx` (C3X) gate definition that `write_qasm` emits in `knapsack/knapsack.c` is reproduced
  from Qiskit's OpenQASM 2 standard library (`qiskit/qasm/libs/qelib1.inc`, gate `c3x`), Copyright
  2017 IBM and its contributors, Apache License 2.0
  (<https://www.apache.org/licenses/LICENSE-2.0>), with the qubit parameters renamed.
- All other code in this repository is original work released under the MIT License below.

## License

MIT License — Copyright (c) 2026 Mark Myers. See `LICENSE`.
