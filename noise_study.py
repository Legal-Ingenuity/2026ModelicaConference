#!/usr/bin/env python3
"""noise_study.py — Aer noise characterization of the Grover knapsack circuit.

Out-of-band by design: the C / OpenModelica solver stays pure (sv_*); here we only
CONSUME the circuit the C exporter produced.  For the chosen study circuit we:

  1. Load qasm_xcheck/<study>.qasm + its metadata sidecar (<study>.meta: measured item
     qubits + the classically-computed marked set, written by qasm_test.c via the
     solver's own is_marked()).
  2. Measure the item register, transpile to a {u, cx} basis.
  3. Apply an all-qubit DEPOLARIZING model of increasing strength (2-qubit error p2,
     1-qubit error p1 = p2/10 — the usual ~10x asymmetry) and sample
     P(success) = Prob(measured subset is marked / optimal-feasible).

Results print as a table AND append to a CSV (default qasm_xcheck/noise_study.csv) for
the manuscript figure.  The final column is a best-of-K reliability proxy, 1-(1-p)^K,
the mitigation the solver's best-of-K wrapper provides.

Examples (run in the project .venv, after ./cmake-build-debug/qasm_test):
    python3 noise_study.py                                   # N=2 (fast, ideal P=1)
    python3 noise_study.py --qasm qasm_xcheck/study4.qasm \\
        --meta qasm_xcheck/study4.meta --label N4 \\
        --shots 400 --method statevector                     # N=4 headline
"""
import argparse
import csv
import os
import sys
import time

from qiskit import ClassicalRegister, transpile
from qiskit.qasm3 import loads
from qiskit_aer import AerSimulator
from qiskit_aer.noise import NoiseModel, depolarizing_error

K = 3                                   # solver default GROVER_K (best-of-K)


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


def depolarizing_model(p1, p2):
    nm = NoiseModel()
    nm.add_all_qubit_quantum_error(depolarizing_error(p1, 1), ["u"])
    nm.add_all_qubit_quantum_error(depolarizing_error(p2, 2), ["cx"])
    return nm


def p_success(counts, marked_bits):
    tot = sum(counts.values())
    hit = sum(c for b, c in counts.items() if b.replace(" ", "") in marked_bits)
    return hit / tot if tot else 0.0


def main():
    ap = argparse.ArgumentParser(description="Aer depolarizing noise study of the Grover knapsack circuit")
    ap.add_argument("--qasm", default="qasm_xcheck/study.qasm")
    ap.add_argument("--meta", default="qasm_xcheck/study.meta")
    ap.add_argument("--label", default="N2")
    ap.add_argument("--shots", type=int, default=20000)
    ap.add_argument("--levels", default="0,1e-4,1e-3,3e-3,1e-2,3e-2,1e-1",
                    help="comma-separated 2-qubit depolarizing rates")
    ap.add_argument("--method", default="automatic",
                    help="Aer method: automatic|statevector|density_matrix")
    ap.add_argument("--csv", default="qasm_xcheck/noise_study.csv")
    args = ap.parse_args()

    levels = [float(x) for x in args.levels.split(",")]
    meta = read_meta(args.meta)
    n = meta["N"][0]
    nq = meta["NQ"][0]
    item_qubits = meta["item_qubits"]
    marked = meta["marked_masks"]
    marked_bits = {format(m, f"0{n}b") for m in marked}     # qiskit MSB-first == item[N-1..0]

    sim = AerSimulator(method=args.method)
    qc = build_measured_circuit(args.qasm, item_qubits, n)
    tqc = transpile(qc, basis_gates=["u", "cx"], optimization_level=0)
    n1 = sum(1 for inst in tqc.data if inst.operation.name == "u")
    n2 = sum(1 for inst in tqc.data if inst.operation.name == "cx")

    print(f"=== Aer depolarizing noise study — Grover knapsack [{args.label}] ===")
    print(f"  instance: N={n}, NQ={nq}, marked {marked} = {sorted(marked_bits)}  (M={len(marked)} of {2**n})")
    print(f"  transpiled to {{u,cx}}: {n1} 1q-gates (u), {n2} 2q-gates (cx); "
          f"shots={args.shots}, method={args.method}")
    print()
    print(f"  {'p2 (2q)':>9} | {'p1 (1q)':>9} | {'P(success)':>10} | {'best-of-'+str(K):>9} | {'sec':>6}")
    print("  " + "-" * 56)

    new_csv = not os.path.exists(args.csv)
    rows = []
    for p2 in levels:
        p1 = p2 / 10.0
        nm = None if p2 == 0.0 else depolarizing_model(p1, p2)
        t0 = time.time()
        res = sim.run(tqc, shots=args.shots, noise_model=nm).result()
        dt = time.time() - t0
        ps = p_success(res.get_counts(), marked_bits)
        bok = 1.0 - (1.0 - ps) ** K
        tag = "  <- ideal" if p2 == 0.0 else ""
        print(f"  {p2:9.1e} | {p1:9.1e} | {ps:10.4f} | {bok:9.4f} | {dt:6.1f}{tag}")
        rows.append(dict(label=args.label, N=n, NQ=nq, n_1q=n1, n_2q=n2,
                         p2=p2, p1=p1, p_success=round(ps, 6), best_of_K=round(bok, 6),
                         shots=args.shots))

    with open(args.csv, "a", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        if new_csv:
            w.writeheader()
        w.writerows(rows)

    print()
    print(f"  P(success) = Prob(measured item register is the marked/optimal subset).")
    print(f"  best-of-K = 1-(1-p)^K reliability of the solver's best-of-K wrapper (K={K}).")
    print(f"  appended {len(rows)} rows to {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
