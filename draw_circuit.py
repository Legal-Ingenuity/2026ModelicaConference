#!/usr/bin/env python3
"""draw_circuit.py — OFF-LINE publication renderer for QDrawDetail levels 4 and 5.

Consumes a gate-level OpenQASM-3 circuit exported by knapsack.c (QDrawDetail level
4 or 5 — the write_qasm exporter) and renders it as a black-&-white
Nielsen-&-Chuang-style circuit figure via Qiskit's LaTeX (qcircuit) drawer +
pdflatex.  This keeps the heavy rendering OUT of the C / OpenModelica path: C emits a
portable text spec; this script (run in the project .venv) turns it into the paper
figure.  It is the drawing sibling of the noise/hardware out-of-band consumers, all
fed by the one shared exporter.

Usage:
  python3 draw_circuit.py --qasm draw_xcheck/circuitFigure.qasm           # -> .tex
  python3 draw_circuit.py --qasm draw_xcheck/circuitFigure.qasm --pdf     # -> .tex + .pdf
  python3 draw_circuit.py --qasm ... --out fig_oracle --fold 16 --pdf

Notes:
  * A level-4 export is ONE representative oracle+diffuser iteration; the BBHT /
    Durr-Hoyer search repeats it.  Caption it as such in the paper.
  * Deep oracles (e.g. N=4, ~1.5k gates) draw as a very wide strip; for a legible
    publication inset point this at a small instance (the N=2 export) or a sub-block,
    and/or raise --fold to wrap the drawing onto multiple rows.

Requires (project .venv): qiskit, qiskit-qasm3-import, pylatexenc; and pdflatex with
the qcircuit LaTeX package for --pdf.
"""
import argparse
import os
import subprocess
import sys


def load_circuit(qasm_path):
    """Parse an OpenQASM-3 file into a Qiskit QuantumCircuit."""
    from qiskit.qasm3 import loads  # provided by qiskit-qasm3-import
    with open(qasm_path) as fh:
        return loads(fh.read())


def main():
    ap = argparse.ArgumentParser(
        description="Render an exported OpenQASM-3 circuit to a B&W Nielsen-Chuang figure.")
    ap.add_argument("--qasm", required=True,
                    help="input OpenQASM-3 file (QDrawDetail level 4/5 export)")
    ap.add_argument("--out", default=None,
                    help="output basename (default: the input file stem)")
    ap.add_argument("--pdf", action="store_true",
                    help="also compile the generated .tex with pdflatex")
    ap.add_argument("--fold", type=int, default=24,
                    help="wrap the drawing every N columns (0 = no wrap)")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="drawing scale passed to circuit.draw()")
    args = ap.parse_args()

    if not os.path.isfile(args.qasm):
        sys.exit(f"draw_circuit: no such file: {args.qasm}")

    out = args.out or os.path.splitext(os.path.basename(args.qasm))[0]
    tex = out + ".tex"

    try:
        qc = load_circuit(args.qasm)
    except Exception as exc:  # noqa: BLE001 — report any load failure plainly
        sys.exit(f"draw_circuit: failed to load {args.qasm}: {exc}\n"
                 f"  (need qiskit + qiskit-qasm3-import in the active .venv)")

    print(f"draw_circuit: loaded {args.qasm} -> "
          f"{qc.num_qubits} qubits, {qc.size()} gates, depth {qc.depth()}")

    # Qiskit's LaTeX drawer emits a standalone qcircuit document (the Nielsen-&-Chuang
    # gate iconography), black-and-white by default.
    try:
        src = qc.draw(output="latex_source",
                      fold=(args.fold if args.fold > 0 else -1),
                      scale=args.scale,
                      plot_barriers=False,
                      initial_state=True)
    except Exception as exc:  # noqa: BLE001
        sys.exit(f"draw_circuit: latex_source draw failed: {exc}\n"
                 f"  (need pylatexenc in the .venv:  pip install pylatexenc)")

    with open(tex, "w") as fh:
        fh.write(src)
    print(f"draw_circuit: wrote {tex}")

    if args.pdf:
        try:
            res = subprocess.run(
                ["pdflatex", "-interaction=nonstopmode", "-halt-on-error", tex],
                capture_output=True, text=True)
        except FileNotFoundError:
            sys.exit("draw_circuit: pdflatex not found on PATH")
        if res.returncode == 0 and os.path.isfile(out + ".pdf"):
            print(f"draw_circuit: wrote {out}.pdf")
        else:
            errs = "\n".join(l for l in res.stdout.splitlines() if l.startswith("!"))
            sys.exit(f"draw_circuit: pdflatex failed (rc={res.returncode}); "
                     f"is the qcircuit package installed?\n{errs[:800]}")


if __name__ == "__main__":
    main()
