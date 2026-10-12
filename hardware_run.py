#!/usr/bin/env python3
"""hardware_run.py — out-of-band IBM Quantum hardware path for the knapsack solver.

The C / OpenModelica solver stays pure (its own sv_* statevector simulator); this
consumer submits the C-exported OpenQASM-3 circuit through qiskit-ibm-runtime's
SamplerV2 primitive — the SAME code path a real IBM QPU run takes.  It is
PATH-PRESERVING: the architecture is demonstrable WITHOUT an IBM account by targeting a
local FakeBackend (a calibrated noisy simulator), and a real-device run is a one-flag
change once an IBM Quantum token is saved.  No quantum-advantage claim — this validates
the interface and reports (noise-degraded) sampling of the optimal subset.

This script does not implement the SolverPath 2 results protocol (solverCircuit.qasm and
.meta in, qpu_results.json out; see README, "Solver paths").  It demonstrates the
submission step only.

It reuses the noise_study.py conventions: a <study>.qasm built by qasm_test.c plus its
<study>.meta sidecar (item qubits + the classically-computed marked/optimal set).

Backends (--backend):
  fake[:Name]   local FakeBackend (default FakeBrisbane), a calibrated noisy
                simulator; NO token needed — the path-preserving demonstration.
  aer           ideal local AerSimulator via the runtime primitive; NO token.
  least-busy    least-busy REAL IBM device (needs a saved account / --token).
  <name>        a specific REAL IBM backend by name (needs a saved account / --token).

Token: pass --token (and optionally --instance), or pre-save once with
  python -c "from qiskit_ibm_runtime import QiskitRuntimeService as S; \\
             S.save_account(token='...', channel='ibm_quantum_platform')"

Examples (project .venv, after ./cmake-build-debug/qasm_test):
  python3 hardware_run.py                                       # N=2 study, FakeBrisbane
  python3 hardware_run.py --backend least-busy --token $IBM_TOKEN   # real hardware
"""
import argparse
import sys

from qiskit import ClassicalRegister
from qiskit.qasm3 import loads
from qiskit.transpiler.preset_passmanagers import generate_preset_pass_manager


def read_meta(path):
    d = {}
    for line in open(path):
        parts = line.split()
        if parts:
            d[parts[0]] = [int(v) for v in parts[1:]]
    return d


def build_measured_circuit(qasm_path, item_qubits, n):
    qc = loads(open(qasm_path).read())
    creg = ClassicalRegister(n, "c")
    qc.add_register(creg)
    for i, q in enumerate(item_qubits):
        qc.measure(q, creg[i])
    return qc


def subset_from_bits(bitstr, n):
    """Decode a qiskit bitstring (MSB-first: leftmost char == creg[n-1] == item[n-1])."""
    b = bitstr.replace(" ", "")
    return frozenset(i for i in range(n) if b[n - 1 - i] == "1")


def get_backend(args):
    """Return (backend, is_real, label); fall back to a FakeBackend if no IBM account."""
    name = args.backend
    if name.startswith("fake"):
        import qiskit_ibm_runtime.fake_provider as fp
        cls = name.split(":", 1)[1] if ":" in name else "FakeBrisbane"
        fb = getattr(fp, cls)()
        return fb, False, f"local {type(fb).__name__} (calibrated noisy simulator; NO real hardware)"
    if name == "aer":
        from qiskit_aer import AerSimulator
        return AerSimulator(), False, "local AerSimulator (ideal; NO real hardware)"

    # real hardware via the runtime service
    from qiskit_ibm_runtime import QiskitRuntimeService
    try:
        if args.token:
            service = QiskitRuntimeService(channel=args.channel, token=args.token,
                                           instance=args.instance)
        else:
            service = QiskitRuntimeService()           # a previously saved account
    except Exception as exc:  # noqa: BLE001
        print(f"hardware_run: no IBM account available ({type(exc).__name__}: {exc}).")
        print("  -> falling back to a local FakeBackend so the path is still demonstrated.")
        from qiskit_ibm_runtime.fake_provider import FakeBrisbane
        return FakeBrisbane(), False, "local FakeBrisbane (fallback; NO real hardware)"

    be = (service.least_busy(operational=True, simulator=False)
          if name == "least-busy" else service.backend(name))
    return be, True, f"REAL IBM backend '{be.name}'"


def main():
    ap = argparse.ArgumentParser(
        description="Submit the exported knapsack circuit via qiskit-ibm-runtime (path-preserving).")
    ap.add_argument("--qasm", default="qasm_xcheck/study.qasm")
    ap.add_argument("--meta", default="qasm_xcheck/study.meta")
    ap.add_argument("--label", default="N2")
    ap.add_argument("--backend", default="fake",
                    help="fake[:Name] | aer | least-busy | <real-backend-name>")
    ap.add_argument("--shots", type=int, default=4096)
    ap.add_argument("--opt-level", type=int, default=1)
    ap.add_argument("--token", default=None, help="IBM Quantum API token (else use a saved account)")
    ap.add_argument("--channel", default="ibm_quantum_platform")
    ap.add_argument("--instance", default=None)
    args = ap.parse_args()

    meta = read_meta(args.meta)
    n, nq = meta["N"][0], meta["NQ"][0]
    item_qubits = meta["item_qubits"]
    marked_subsets = {frozenset(i for i in range(n) if (m >> i) & 1) for m in meta["marked_masks"]}

    qc = build_measured_circuit(args.qasm, item_qubits, n)
    backend, is_real, blabel = get_backend(args)

    print(f"=== IBM-runtime hardware path — Grover knapsack [{args.label}] ===")
    print(f"  circuit : N={n}, NQ={nq}, item qubits {item_qubits}; "
          f"marked/optimal subsets {[sorted(s) for s in marked_subsets]}")
    print(f"  backend : {blabel}")
    if not is_real:
        print("  NOTE    : PATH-PRESERVING demo — identical SamplerV2 code path, no real QPU, no advantage claim.")

    # transpile to the backend's ISA, then sample with the SamplerV2 primitive
    pm = generate_preset_pass_manager(optimization_level=args.opt_level, backend=backend)
    isa = pm.run(qc)
    twoq = sum(1 for inst in isa.data if inst.operation.num_qubits == 2)
    print(f"  transpiled to backend ISA: depth {isa.depth()}, {twoq} 2-qubit gates; shots={args.shots}")

    from qiskit_ibm_runtime import SamplerV2 as Sampler
    print("  submitting ...")
    job = Sampler(mode=backend).run([isa], shots=args.shots)
    counts = job.result()[0].data.c.get_counts()

    # decode the item register and score against the optimal-feasible set
    tot = sum(counts.values())
    hit = sum(c for b, c in counts.items() if subset_from_bits(b, n) in marked_subsets)
    top_bits, top_cnt = max(counts.items(), key=lambda kv: kv[1])
    top_subset = subset_from_bits(top_bits, n)
    p_succ = hit / tot if tot else 0.0

    print()
    print(f"  most-frequent outcome : items {sorted(top_subset)}  "
          f"({top_cnt}/{tot} shots, {100 * top_cnt / tot:.1f}%)")
    print(f"  P(success) = Prob(measured subset is optimal-feasible) = {p_succ:.4f}")
    ok = top_subset in marked_subsets
    print(f"  {'OPTIMAL recovered' if ok else 'sub-optimal top outcome (noise)'}; "
          f"{'REAL-hardware' if is_real else 'simulated'} sampling.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
