#!/usr/bin/env python3
"""verify_qasm.py — out-of-band verification of the C OpenQASM-3 exporter.

Loads each .qasm exported by qasm_test.c (knapsack.c write_qasm), computes
Qiskit's Statevector, and compares it — global-phase-tolerant fidelity — to the
sv_* statevector that qasm_test.c wrote alongside it.

A PASS means BOTH:
  (a) write_qasm produced valid, semantically-correct OpenQASM 3 (Qiskit parses
      it and the custom ccz/mcx gate defs reproduce the intended unitary), and
  (b) our pure-C statevector simulator agrees with Qiskit on the same circuit.

This is the export architecture in miniature: the C side only EXPORTS; Qiskit
only CONSUMES.  Run from the project root, in the project .venv, after building:

    ./cmake-build-debug/qasm_test
    python3 verify_qasm.py
"""
import sys
import numpy as np
from qiskit.qasm3 import loads
from qiskit.quantum_info import Statevector

CASES = [
    ("kitchen sink", "qasm_xcheck/kitchen.qasm", "qasm_xcheck/kitchen.sv"),
    ("grover N=2",    "qasm_xcheck/grover.qasm",  "qasm_xcheck/grover.sv"),
]


def load_our_sv(path):
    """Read the C-side statevector file ('re im' per line) into a complex array."""
    d = np.atleast_2d(np.loadtxt(path))
    return d[:, 0] + 1j * d[:, 1]


def fidelity(a, b):
    """|<a|b>|^2 / (<a|a><b|b>) — 1.0 iff equal up to global phase."""
    if a.shape != b.shape:
        return -1.0
    ov = np.vdot(a, b)
    return abs(ov) ** 2 / (np.vdot(a, a).real * np.vdot(b, b).real)


def main():
    fails = 0
    print("=== qiskit round-trip of the C exporter (fidelity vs our sv_*) ===")
    for name, qpath, spath in CASES:
        try:
            with open(qpath) as fh:
                qc = loads(fh.read())
            qsv = Statevector(qc).data
            osv = load_our_sv(spath)
            fid = fidelity(qsv, osv)
            ok = abs(fid - 1.0) < 1e-9
            print(f"  {'PASS' if ok else 'FAIL'}  {name:13s}  "
                  f"fidelity={fid:.12f}  (qiskit dim {len(qsv)}, our dim {len(osv)})")
            fails += not ok
        except Exception as exc:                      # noqa: BLE001 (report any failure)
            print(f"  FAIL  {name:13s}  exception: {exc}")
            fails += 1
    print("=== " + ("ALL PASS" if fails == 0 else f"{fails} FAILED") + " ===")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
