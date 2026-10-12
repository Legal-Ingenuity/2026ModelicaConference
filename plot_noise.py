#!/usr/bin/env python3
"""plot_noise.py — render the noise-study CSV as a publication figure.

Out-of-band: reads qasm_xcheck/noise_study.csv (written by noise_study.py) and plots
P(success) versus the two-qubit depolarizing error rate p2, one curve per instance label
(e.g. N2, N4), with the best-of-K reliability proxy as a dashed companion and the
fully-mixed asymptote 1/2^N as a dotted reference.  The leftward shift of the deeper
(N4) knee relative to N2 is the depth-scaling law made visual.  No effect on the C /
OpenModelica path.

    python3 plot_noise.py                       # -> qasm_xcheck/noise_study.{png,pdf}
"""
import argparse
import csv
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")                  # headless; write files only
import matplotlib.pyplot as plt        # noqa: E402


def main():
    ap = argparse.ArgumentParser(description="Plot the noise-study CSV.")
    ap.add_argument("--csv", default="qasm_xcheck/noise_study.csv")
    ap.add_argument("--out", default="qasm_xcheck/noise_study")
    args = ap.parse_args()

    try:
        rows = list(csv.DictReader(open(args.csv)))
    except FileNotFoundError:
        sys.exit(f"plot_noise: no such file: {args.csv} (run noise_study.py first)")
    if not rows:
        sys.exit(f"plot_noise: no data rows in {args.csv}")

    groups = defaultdict(list)
    for r in rows:
        groups[r["label"]].append(r)

    floor = 3e-6                       # show the ideal (p2=0) point on a log axis
    fig, ax = plt.subplots(figsize=(6.2, 4.3))

    for label in sorted(groups):
        grp = sorted(groups[label], key=lambda r: float(r["p2"]))
        n = int(grp[0]["N"])
        n2 = int(grp[0]["n_2q"])
        x = [max(float(r["p2"]), floor) for r in grp]
        ps = [float(r["p_success"]) for r in grp]
        bk = [float(r["best_of_K"]) for r in grp]
        line, = ax.plot(x, ps, "o-", label=f"{label} (N={n}, {n2} cx): single")
        ax.plot(x, bk, "^--", color=line.get_color(), alpha=0.6,
                label=f"{label}: best-of-3")
        ax.axhline(1.0 / 2 ** n, color=line.get_color(), ls=":", lw=0.8, alpha=0.5)

    ax.set_xscale("log")
    ax.set_xlim(left=2e-6)
    ax.set_ylim(-0.02, 1.02)
    ax.set_xlabel(r"two-qubit depolarizing error rate $p_2$  (leftmost point: ideal, $p_2=0$)")
    ax.set_ylabel("P(success) = Prob(measured subset is optimal-feasible)")
    ax.set_title("Noise degradation of the Grover knapsack oracle\n"
                 r"depth-scaling: deeper circuit $\Rightarrow$ lower noise tolerance")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.legend(fontsize=8, loc="lower left")
    fig.tight_layout()

    for ext in ("png", "pdf"):
        fig.savefig(f"{args.out}.{ext}", dpi=150)
        print(f"plot_noise: wrote {args.out}.{ext}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
