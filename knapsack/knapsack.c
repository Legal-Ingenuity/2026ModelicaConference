/*
 * knapsack.c — quantum 0/1 knapsack solver for OpenModelica, built on the Qiskit C API.
 *
 * Modules (in file order):
 *   1. Statevector simulator     sv_alloc, sv_h, sv_x, sv_y, sv_phase, sv_cx, sv_mcx, sv_mcz,
 *                                simulate_circuit — a pure-C simulator for the gates this file emits.
 *   2. Circuit construction      emit_* — adders, comparators, the oracle (feasible AND value >= T)
 *                                and the Grover diffuser, appended to a QkCircuit.
 *   3. Quantum search            grng_*, setup_gctx, build_grover_circuit, run_and_measure,
 *                                grover_sample, grover_search (BBHT), durr_hoyer, grover_knapsack.
 *   4. Solver paths              solver_sim (in-process), solver_aer / solver_qpu (export the circuit
 *                                as OpenQASM 3, wait for a results file), solve_knapsack (the switch).
 *   5. Circuit writers           write_latex_circuit, write_qasm, write_latex_circuit_expanded,
 *                                write_circuit_drawing (QDrawDetail levels 1-5).
 *   6. Entry point               callKnapsack — the function ModKnapsack.mo calls.
 *
 * References: Grover (1996); Boyer, Brassard, Hoyer, Tapp (1998); Durr and Hoyer (1996);
 * Cuccaro et al. (2004); Gilliam, Woerner, Gonciulea (2021). See ALGORITHM_REFERENCES.pdf.
 *
 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-11
 */

#include <complex.h>
#include <math.h>
#include <qiskit.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

/* Define M_PI if the compiler/platform does not expose it. */
#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

/* ────────────────────────────────────────────────────────────────────────── */
/* Constants                                                                   */
/* ────────────────────────────────────────────────────────────────────────── */

#define MAX_ITEMS 20   /* bound on the item arrays only; the quantum solver accepts
                          N <= KNAPSACK_NMAX (defined below), which is smaller */

/* Output filename for the LaTeX circuit diagram. */
#define LATEX_OUTPUT_FILE "printCircuitFile.txt"
#define STATS_OUTPUT_FILE "groverStats.txt"

/* ════════════════════════════════════════════════════════════════════════════
 * Section 1: Pure-C Statevector Simulator
 * ════════════════════════════════════════════════════════════════════════════ */

/* The sv_* routines implement the standard index-pair update: for a gate on qubit t, every
 * amplitude whose bit t is 0 is paired with the one whose bit t is 1 and the 2x2 matrix is
 * applied to the pair; multi-controlled gates mask the controls first. See ALGORITHM_REFERENCES.pdf
 * section G (Nielsen & Chuang 2010 §4.2–4.3; De Raedt et al. 2007/2019; qHiPSTER 2016; QuEST 2019). */

/*
 * sv_alloc — Allocate an n-qubit statevector initialised to |0...0>.
 * Inputs : n: number of qubits
 * Outputs: pointer to 2^n complex amplitudes (caller frees)
 */
static double complex *sv_alloc(int n)
{
    size_t sz = (size_t)1 << n;
    double complex *sv = calloc(sz, sizeof(double complex));
    if (!sv) { fputs("sv_alloc: out of memory\n", stderr); exit(EXIT_FAILURE); }
    sv[0] = 1.0 + 0.0 * I;
    return sv;
}

/*
 * sv_h — Apply a Hadamard gate to qubit t.
 * Inputs : sv, n: statevector and its qubit count; t: target qubit
 * Outputs: sv updated in place
 */
static void sv_h(double complex *sv, int n, uint32_t t)
{
    double inv_r2 = 1.0 / sqrt(2.0);
    for (size_t i = 0; i < (size_t)1 << n; i++) {
        if ((i >> t) & 1) continue;
        size_t j = i | ((size_t)1 << t);
        double complex a = sv[i], b = sv[j];
        sv[i] = (a + b) * inv_r2;
        sv[j] = (a - b) * inv_r2;
    }
}

/*
 * sv_x — Apply an X (NOT) gate to qubit t.
 * Inputs : sv, n, t
 * Outputs: sv updated in place
 */
static void sv_x(double complex *sv, int n, uint32_t t)
{
    for (size_t i = 0; i < (size_t)1 << n; i++) {
        if ((i >> t) & 1) continue;
        size_t j = i | ((size_t)1 << t);
        double complex tmp = sv[i]; sv[i] = sv[j]; sv[j] = tmp;
    }
}

/*
 * sv_y — Apply a Y gate to qubit t.
 * Inputs : sv, n, t
 * Outputs: sv updated in place
 */
static void sv_y(double complex *sv, int n, uint32_t t)
{
    for (size_t i = 0; i < (size_t)1 << n; i++) {
        if ((i >> t) & 1) continue;
        size_t j = i | ((size_t)1 << t);
        double complex a = sv[i], b = sv[j];
        sv[i] = -b * I;
        sv[j] =  a * I;
    }
}


/*
 * sv_phase — Apply the phase gate diag(1, e^{i theta}) to qubit t.
 * Inputs : sv, n, t; theta: phase in radians
 * Outputs: sv updated in place
 */
static void sv_phase(double complex *sv, int n, uint32_t t, double theta)
{
    double complex ph = cexp(theta * I);
    for (size_t i = 0; i < (size_t)1 << n; i++)
        if ((i >> t) & 1)
            sv[i] *= ph;
}

/*
 * sv_cx — Apply a controlled-NOT with control c and target t.
 * Inputs : sv, n; c, t: control and target qubits
 * Outputs: sv updated in place
 */
static void sv_cx(double complex *sv, int n, uint32_t c, uint32_t t)
{
    for (size_t i = 0; i < (size_t)1 << n; i++) {
        if (!((i >> c) & 1)) continue;
        if ((i >> t)  & 1)  continue;
        size_t j = i | ((size_t)1 << t);
        double complex tmp = sv[i]; sv[i] = sv[j]; sv[j] = tmp;
    }
}



/*
 * sv_mcx — Apply a multi-controlled NOT (generalised Toffoli).
 * Inputs : sv, n; controls, nctrl: control qubits; target
 * Outputs: sv updated in place
 */
static void sv_mcx(double complex *sv, int n,
                   const uint32_t *controls, uint32_t nctrl, uint32_t target)
{
    size_t cmask = 0;
    for (uint32_t k = 0; k < nctrl; k++)
        cmask |= (size_t)1 << controls[k];
    size_t tbit = (size_t)1 << target;

    for (size_t i = 0; i < ((size_t)1 << n); i++) {
        if ((i & cmask) != cmask) continue;   /* a control is 0 → leave alone   */
        if (i & tbit)             continue;   /* visit each pair once (t-bit = 0)*/
        size_t j = i | tbit;                  /* partner: target flipped to 1    */
        double complex tmp = sv[i]; sv[i] = sv[j]; sv[j] = tmp;
    }
}


/*
 * sv_mcz — Apply a multi-controlled Z: the phase -1 on the all-ones state of the listed qubits.
 * Inputs : sv, n; qubits, nq: the qubits
 * Outputs: sv updated in place
 */
static void sv_mcz(double complex *sv, int n,
                   const uint32_t *qubits, uint32_t nq)
{
    size_t mask = 0;
    for (uint32_t k = 0; k < nq; k++)
        mask |= (size_t)1 << qubits[k];

    for (size_t i = 0; i < ((size_t)1 << n); i++)
        if ((i & mask) == mask)
            sv[i] = -sv[i];
}

/* ════════════════════════════════════════════════════════════════════════════
 * Section 2: Circuit Simulation via the Qiskit C API
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * simulate_circuit — Run every instruction of a QkCircuit on the statevector.
 * Inputs : qc: the circuit; sv, n: statevector and qubit count
 * Outputs: number of gates applied (>= 0), or -1 on an unhandled gate; sv holds the final state
 */
static int simulate_circuit(const QkCircuit *qc, double complex *sv, int n)
{
    size_t num_inst = qk_circuit_num_instructions(qc);
    int applied = 0;

    for (size_t idx = 0; idx < num_inst; idx++) {
        if (qk_circuit_instruction_kind(qc, idx) != QkOperationKind_Gate)
            continue;

        QkCircuitInstruction inst;
        qk_circuit_get_instruction(qc, idx, &inst);

        uint32_t q0 = (inst.num_qubits > 0) ? inst.qubits[0] : 0;
        uint32_t q1 = (inst.num_qubits > 1) ? inst.qubits[1] : 0;

        double p0 = 0.0;
        if (inst.num_params > 0 && inst.params && inst.params[0]) {
            char *ps = qk_param_str(inst.params[0]);
            if (ps) { p0 = strtod(ps, NULL); qk_str_free(ps); }
        }

        if      (!strcmp(inst.name, "h"))
            { sv_h (sv, n, q0);               applied++; }
        else if (!strcmp(inst.name, "x"))
            { sv_x (sv, n, q0);               applied++; }
        else if (!strcmp(inst.name, "y"))
            { sv_y (sv, n, q0);               applied++; }
        else if (!strcmp(inst.name, "z"))
            { sv_phase(sv, n, q0, M_PI);         applied++; }
        else if (!strcmp(inst.name, "s"))
            { sv_phase(sv, n, q0,  M_PI / 2.0);  applied++; }
        else if (!strcmp(inst.name, "sdg"))
            { sv_phase(sv, n, q0, -M_PI / 2.0);  applied++; }
        else if (!strcmp(inst.name, "t"))
            { sv_phase(sv, n, q0,  M_PI / 4.0);  applied++; }
        else if (!strcmp(inst.name, "tdg"))
            { sv_phase(sv, n, q0, -M_PI / 4.0);  applied++; }
        else if (!strcmp(inst.name, "cx"))
            { sv_cx(sv, n, q0, q1);           applied++; }
        else if (!strcmp(inst.name, "rz"))
            { sv_phase(sv, n, q0, p0);           applied++; }
        else if (!strcmp(inst.name, "phase") || !strcmp(inst.name, "p"))
            { sv_phase(sv, n, q0, p0);           applied++; }

        else if (!strcmp(inst.name, "ccx") || !strcmp(inst.name, "mcx")) {
            uint32_t nctrl = inst.num_qubits - 1;
            sv_mcx(sv, n, inst.qubits, nctrl, inst.qubits[nctrl]);
            applied++;
        }
        /* CZ emits "cz" (2 qubits), CCZ emits "ccz" (3). Both are symmetric
         * phase flips, so sv_mcz takes the whole qubit list as joint controls. */
        else if (!strcmp(inst.name, "cz") || !strcmp(inst.name, "ccz")) {
            sv_mcz(sv, n, inst.qubits, inst.num_qubits);
            applied++;
        }
        else {
            fprintf(stderr,
                "simulate_circuit: UNHANDLED gate '%s' (%u qubit(s)) at "
                "instruction %zu — statevector would be incorrect; aborting.\n",
                inst.name, inst.num_qubits, idx);
            qk_circuit_instruction_clear(&inst);
            return -1;
        }

        qk_circuit_instruction_clear(&inst);
    }
    return applied;
}


/* "emit" is used throughout in the compiler sense: an emit_* routine appends gates to the
 * circuit under construction. It is unrelated to a quantum emitter (a physical photon source). */

/* Thin wrappers around qk_circuit_gate for the three gates the adder emits.
 * Qiskit's qubit order is controls-first, target-last (verified by probe):
 *   CX  → [control, target]      CCX → [c0, c1, target]      C3X → [c0,c1,c2,target] */

/*
 * emit_cx — Append a CX gate.
 * Inputs : qc; c, t: control, target
 * Outputs: qc extended
 */
static void emit_cx(QkCircuit *qc, uint32_t c, uint32_t t)
{
    uint32_t q[2] = { c, t };
    qk_circuit_gate(qc, QkGate_CX, q, NULL);
}

/*
 * emit_ccx — Append a CCX (Toffoli) gate.
 * Inputs : qc; c0, c1, t
 * Outputs: qc extended
 */
static void emit_ccx(QkCircuit *qc, uint32_t c0, uint32_t c1, uint32_t t)
{
    uint32_t q[3] = { c0, c1, t };
    qk_circuit_gate(qc, QkGate_CCX, q, NULL);
}

/*
 * emit_c3x — Append a triply-controlled X gate.
 * Inputs : qc; c0, c1, c2, t
 * Outputs: qc extended
 */
static void emit_c3x(QkCircuit *qc, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t t)
{
    uint32_t q[4] = { c0, c1, c2, t };
    qk_circuit_gate(qc, QkGate_C3X, q, NULL);
}


/*
 * emit_carry — Append the controlled carry of one adder bit: cout ^= ctrl AND majority(a_i, cost_i, cin).
 * Inputs : qc; a_i: constant bit; ctrl: control qubit; cost_i: register bit; has_cin, cin: carry-in (optional); cout: carry-out qubit
 * Outputs: qc extended
 */
static void emit_carry(QkCircuit *qc, int a_i, uint32_t ctrl,
                       uint32_t cost_i, int has_cin, uint32_t cin, uint32_t cout)
{
    if (!has_cin) {
        /* LSB: carry_in ≡ 0, so majority(a_i, cost_i, 0) = a_i AND cost_i. */
        if (a_i)
            emit_ccx(qc, ctrl, cost_i, cout);   /* cout ^= ctrl·cost_i           */
        /* a_i == 0 → majority is 0 → emit nothing.                              */
    } else if (!a_i) {
        /* majority(0, cost_i, cin) = cost_i AND cin. */
        emit_c3x(qc, ctrl, cost_i, cin, cout);  /* cout ^= ctrl·cost_i·cin       */
    } else {
        emit_cx (qc, ctrl, cost_i);             /* ¬cost_i (temporarily)         */
        emit_cx (qc, ctrl, cin);                /* ¬cin    (temporarily)         */
        emit_c3x(qc, ctrl, cost_i, cin, cout);  /* cout ^= ctrl·¬cost_i·¬cin      */
        emit_cx (qc, ctrl, cout);               /* cout ^= ctrl                   */
        emit_cx (qc, ctrl, cost_i);             /* restore cost_i                 */
        emit_cx (qc, ctrl, cin);                /* restore cin                    */
    }
}


/*
 * emit_controlled_const_add — Append 'register += constant' when ctrl is |1> (ripple-carry adder).
 * Inputs : qc; ctrl; constant; cost: register qubits; W: register width; carry: W-1 scratch qubits
 * Outputs: qc extended
 */
static void emit_controlled_const_add(QkCircuit *qc, uint32_t ctrl,
        uint32_t constant, const uint32_t *cost, uint32_t W, const uint32_t *carry)
{
    /* ── Forward pass: compute every carry-out carry[0..W-2]. ──────────────────
     * The cost register is only READ here, never written, so each carry is a
     * pure function of the original cost bits (which makes the uncompute valid). */
    for (uint32_t i = 0; i + 1 < W; i++) {        /* i = 0 … W-2  (carry out of bit i) */
        int a_i = (int)((constant >> i) & 1u);
        if (i == 0)
            emit_carry(qc, a_i, ctrl, cost[0], 0, 0u,          carry[0]);
        else
            emit_carry(qc, a_i, ctrl, cost[i], 1, carry[i - 1], carry[i]);
    }

    for (int i = (int)W - 1; i >= 0; i--) {
        int a_i = (int)((constant >> i) & 1u);

        /* (a) Uncompute carry out of bit i (none exists for the top bit). */
        if (i + 1 < (int)W) {
            if (i == 0)
                emit_carry(qc, a_i, ctrl, cost[0], 0, 0u,              carry[0]);
            else
                emit_carry(qc, a_i, ctrl, cost[i], 1, carry[i - 1],    carry[i]);
        }

        /* (b) Write the sum bit:  cost[i] ^= ctrl AND ( a_i XOR carry_in_i ). */
        if (i == 0) {
            /* carry_in ≡ 0 → cost[0] ^= ctrl·a_0. */
            if (a_i)
                emit_cx(qc, ctrl, cost[0]);
        } else {
            uint32_t cin = carry[i - 1];
            if (!a_i) {
                emit_ccx(qc, ctrl, cin, cost[i]);   /* ^= ctrl·cin                    */
            } else {
                emit_cx (qc, ctrl, cost[i]);        /* ^= ctrl …                      */
                emit_ccx(qc, ctrl, cin, cost[i]);   /* … ^= ctrl·cin  ⇒  ctrl·¬cin    */
            }
        }
    }
}


/*
 * emit_x — Append a plain X gate (used by the uncontrolled OR-carry to negate its inputs in place).
 * Inputs : qc; t
 * Outputs: qc extended
 */
static void emit_x(QkCircuit *qc, uint32_t t)
{
    uint32_t q[1] = { t };
    qk_circuit_gate(qc, QkGate_X, q, NULL);
}


/*
 * emit_carry_uc — Append the uncontrolled carry of one adder bit (used by the comparator).
 * Inputs : qc; a_i: constant bit; reg_i: register bit; has_cin, cin; cout
 * Outputs: qc extended
 */
static void emit_carry_uc(QkCircuit *qc, int a_i, uint32_t reg_i,
                          int has_cin, uint32_t cin, uint32_t cout)
{
    if (!has_cin) {
        /* LSB: carry_in ≡ 0, so majority(a_i, reg_i, 0) = a_i AND reg_i. */
        if (a_i)
            emit_cx(qc, reg_i, cout);            /* cout ^= reg_i                   */
        /* a_i == 0 → majority is 0 → emit nothing.                                 */
    } else if (!a_i) {
        /* majority(0, reg_i, cin) = reg_i AND cin. */
        emit_ccx(qc, reg_i, cin, cout);          /* cout ^= reg_i·cin               */
    } else {
        /* majority(1, reg_i, cin) = reg_i OR cin = ¬(¬reg_i ∧ ¬cin).
         * Flip both inputs, AND them into cout, add 1, unflip — leaving
         *   cout ^= ¬(¬reg_i·¬cin) = reg_i OR cin,  reg_i and cin restored.        */
        emit_x  (qc, reg_i);                     /* ¬reg_i (temporarily)            */
        emit_x  (qc, cin);                       /* ¬cin   (temporarily)            */
        emit_ccx(qc, reg_i, cin, cout);          /* cout ^= ¬reg_i·¬cin             */
        emit_x  (qc, cout);                      /* cout ^= 1  ⇒  reg_i OR cin      */
        emit_x  (qc, reg_i);                     /* restore reg_i                   */
        emit_x  (qc, cin);                       /* restore cin                     */
    }
}


/*
 * emit_compare_const — Append a comparator: flag ^= carry-out of (register + K) over m bits.
 * Inputs : qc; reg, m: register qubits and width; carry: m scratch qubits; K: constant; flag: result qubit; flip_flag: invert the result
 * Outputs: qc extended
 */
static void emit_compare_const(QkCircuit *qc,
        const uint32_t *reg, uint32_t m, const uint32_t *carry,
        uint32_t K, uint32_t flag, int flip_flag)
{
    for (uint32_t i = 0; i < m; i++) {
        int a_i = (int)((K >> i) & 1u);
        if (i == 0) emit_carry_uc(qc, a_i, reg[0], 0, 0u,           carry[0]);
        else        emit_carry_uc(qc, a_i, reg[i], 1, carry[i - 1], carry[i]);
    }

    /* ── Copy the sign (MSB carry-out) into the flag BEFORE uncomputing it. ──── */
    emit_cx(qc, carry[m - 1], flag);             /* flag ^= carry-out               */
    if (flip_flag)
        emit_x(qc, flag);                        /* flag ^= 1  ⇒  flag ^= ¬carry-out */

    for (int i = (int)m - 1; i >= 0; i--) {
        int a_i = (int)((K >> i) & 1u);
        if (i == 0) emit_carry_uc(qc, a_i, reg[0], 0, 0u,           carry[0]);
        else        emit_carry_uc(qc, a_i, reg[i], 1, carry[i - 1], carry[i]);
    }
}

/* Mask of the low m bits (m in 1..32); guards against the 1u<<32 UB at m == 32. */

/*
 * low_mask — Bitmask of the low m bits.
 * Inputs : m: 1..32
 * Outputs: mask
 */
static uint32_t low_mask(uint32_t m)
{
    return (m >= 32) ? 0xFFFFFFFFu : ((1u << m) - 1u);
}


/*
 * emit_flag_leq — Append 'flag ^= (register <= W)'.
 * Inputs : qc; reg, m; carry; W: 0 .. 2^m-1 (solve_knapsack clamps the budget to the cost total, so this holds); flag
 * Outputs: qc extended
 */
static void emit_flag_leq(QkCircuit *qc, const uint32_t *reg, uint32_t m,
                          const uint32_t *carry, uint32_t W, uint32_t flag)
{
    uint32_t K = (low_mask(m) + 1u - (W + 1u)) & low_mask(m);   /* (2ᵐ − (W+1)) mod 2ᵐ */
    emit_compare_const(qc, reg, m, carry, K, flag, /*flip_flag=*/1);
}


/*
 * emit_flag_geq — Append 'flag ^= (register >= T)'.
 * Inputs : qc; reg, m; carry; T: 1 .. 2^m (the callers guarantee T >= 1; T = 0 would never set the flag); flag
 * Outputs: qc extended
 */
static void emit_flag_geq(QkCircuit *qc, const uint32_t *reg, uint32_t m,
                          const uint32_t *carry, uint32_t T, uint32_t flag)
{
    uint32_t K = (low_mask(m) + 1u - T) & low_mask(m);          /* (2ᵐ − T) mod 2ᵐ */
    emit_compare_const(qc, reg, m, carry, K, flag, /*flip_flag=*/0);
}


/*
 * emit_cz — Append a controlled-Z gate: the phase −1 exactly on the |1,1> branch of (a,b); used to mark (feas ∧ valok).
 * Inputs : qc; a, b
 * Outputs: qc extended
 */
static void emit_cz(QkCircuit *qc, uint32_t a, uint32_t b)
{
    uint32_t q[2] = { a, b };
    qk_circuit_gate(qc, QkGate_CZ, q, NULL);
}


/*
 * emit_controlled_const_sub — Append 'register -= constant' when ctrl is |1> (the adder with the two's complement).
 * Inputs : qc; ctrl; constant; reg, W, carry
 * Outputs: qc extended
 */
static void emit_controlled_const_sub(QkCircuit *qc, uint32_t ctrl,
        uint32_t constant, const uint32_t *reg, uint32_t W, const uint32_t *carry)
{
    uint32_t k = (low_mask(W) + 1u - (constant & low_mask(W))) & low_mask(W);
    emit_controlled_const_add(qc, ctrl, k, reg, W, carry);
}


/* Why an oracle, and why it is built as compute → mark → uncompute.
 * 0/1 knapsack is NP-hard, but checking one candidate subset is cheap: add its costs and
 * values and compare them with the budget and the threshold. That polynomial-time check is the
 * problem's verifier, and the oracle is that verifier written as a reversible circuit. Grover's
 * search does not avoid the 2^N candidates; it runs the verifier on all of them in superposition
 * and needs only about sqrt(2^N) oracle calls instead of 2^N (Grover 1996; Boyer et al. 1998).
 * The verifier's one-bit answer is applied as a phase of -1 on the accepted subsets (the mark);
 * the sums and flags are then uncomputed so that no ancilla stays entangled with the item
 * register, which would spoil the interference the diffuser depends on. */

/*
 * emit_oracle — Append the oracle: compute each subset's cost and value, flip the phase of the subsets that fit the budget and reach the threshold, then uncompute.
 * Inputs : qc; item, N: item qubits; costs, values: item data; cost_reg, m_cost; value_reg, m_val: accumulator registers; carry: max(m_cost, m_val) scratch qubits; feas, valok: flag qubits; W: budget; T: value threshold
 * Outputs: qc extended; every ancilla (registers, carries, flags) is back to |0> afterwards
 */
static void emit_oracle(QkCircuit *qc,
        const uint32_t *item, int N,
        const uint32_t *costs, const uint32_t *values,
        const uint32_t *cost_reg,  uint32_t m_cost,
        const uint32_t *value_reg, uint32_t m_val,
        const uint32_t *carry,
        uint32_t feas, uint32_t valok,
        uint32_t W, uint32_t T)
{
    /* ── FORWARD: accumulate the two sums, then derive the two predicate flags. ── */
    for (int i = 0; i < N; i++)
        emit_controlled_const_add(qc, item[i], costs[i],  cost_reg,  m_cost, carry);
    for (int i = 0; i < N; i++)
        emit_controlled_const_add(qc, item[i], values[i], value_reg, m_val,  carry);
    emit_flag_leq(qc, cost_reg,  m_cost, carry, W, feas);    /* feas  = (cost  ≤ W) */
    emit_flag_geq(qc, value_reg, m_val,  carry, T, valok);   /* valok = (value ≥ T) */

    /* ── MARK: phase −1 exactly when feas ∧ valok. ── */
    emit_cz(qc, feas, valok);

    /* ── UNCOMPUTE (mirror image): flags are self-inverse → call again; adders are
     *    not → reverse with complement-subtract.  All ancillas return to |0…0⟩. ── */
    emit_flag_geq(qc, value_reg, m_val,  carry, T, valok);
    emit_flag_leq(qc, cost_reg,  m_cost, carry, W, feas);
    for (int i = 0; i < N; i++)
        emit_controlled_const_sub(qc, item[i], values[i], value_reg, m_val,  carry);
    for (int i = 0; i < N; i++)
        emit_controlled_const_sub(qc, item[i], costs[i],  cost_reg,  m_cost, carry);
}


/* What the diffuser is and what it does.
 * A Grover iteration has two halves. The oracle (above) marks the subsets that pass the check
 * by flipping the sign of their amplitudes; nothing is measurable yet, because a sign flip
 * does not change any probability. The diffuser turns those signs into probability: it
 * reflects every amplitude about the average amplitude ("inversion about the mean"), so the
 * marked, negated amplitudes end up above the average and the unmarked ones slightly below.
 * Repeating oracle + diffuser about sqrt(2^N / M) times, for M marked subsets, drives nearly
 * all of the probability onto the marked subsets, which is what the measurement then finds.
 * As a circuit the reflection is H on every item qubit, X on every item qubit, a phase of -1
 * on the all-ones state (the multi-controlled Z built by emit_cnz), then X and H again
 * (Grover 1996; Nielsen & Chuang 2010 §6.1.2). The multi-controlled Z for N >= 5 follows the
 * Toffoli-ladder construction of Barenco et al. 1995 (ALGORITHM_REFERENCES.pdf items 1, 8, 9). */

/*
 * emit_h — Append a Hadamard gate.
 * Inputs : qc; q
 * Outputs: qc extended
 */
static void emit_h(QkCircuit *qc, uint32_t q)
{
    uint32_t a[1] = { q };
    qk_circuit_gate(qc, QkGate_H, a, NULL);
}

/*
 * emit_z — Append a Z gate.
 * Inputs : qc; q
 * Outputs: qc extended
 */
static void emit_z(QkCircuit *qc, uint32_t q)
{
    uint32_t a[1] = { q };
    qk_circuit_gate(qc, QkGate_Z, a, NULL);
}

/*
 * emit_ccz — Append a doubly-controlled Z gate.
 * Inputs : qc; a, b, c
 * Outputs: qc extended
 */
static void emit_ccz(QkCircuit *qc, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t q[3] = { a, b, c };
    qk_circuit_gate(qc, QkGate_CCZ, q, NULL);
}

/* C^(n-1)Z on the n listed qubits: multiply by −1 exactly on |1…1>.  Native up to
 * 3 controls; n=4 uses C³Z = H·C3X·H (C3X = native "mcx"); n>=5 uses a Toffoli
 * AND-ladder into n-3 clean ancillas, a CCZ, and the ladder in reverse.  The gate is
 * symmetric, so which qubits serve as the targets is immaterial. */

/*
 * emit_cnz — Append a multi-controlled Z over the n listed qubits (phase -1 on their all-ones state).
 * Inputs : qc; q, n: the qubits; anc, n_anc: clean |0> ancillas (n-3 are used when n >= 5, none otherwise)
 * Outputs: 0 and qc extended; -1 and nothing emitted (message on stderr) if n >= 5 and the ancillas are too few
 */
static int emit_cnz(QkCircuit *qc, const uint32_t *q, int n, const uint32_t *anc, int n_anc)
{
    if      (n == 1) emit_z (qc, q[0]);
    else if (n == 2) emit_cz(qc, q[0], q[1]);
    else if (n == 3) emit_ccz(qc, q[0], q[1], q[2]);
    else if (n == 4) {
        emit_h  (qc, q[3]);
        emit_c3x(qc, q[0], q[1], q[2], q[3]);   /* C3X = "mcx" */
        emit_h  (qc, q[3]);
    } else {
        /* n >= 5: anc[0] = q0·q1, anc[i] = q[i+1]·anc[i-1] for i < n-3, then
         * CCZ(anc[n-4], q[n-2], q[n-1]) = -1 exactly on |1…1>, then uncompute. */
        int need = n - 3;
        if (!anc || n_anc < need) {
            fprintf(stderr, "emit_cnz: n=%d needs %d clean ancilla(s), %d available — nothing emitted\n",
                    n, need, n_anc);
            return -1;
        }
        emit_ccx(qc, q[0], q[1], anc[0]);
        for (int i = 1; i < need; i++) emit_ccx(qc, q[i + 1], anc[i - 1], anc[i]);
        emit_ccz(qc, anc[need - 1], q[n - 2], q[n - 1]);
        for (int i = need - 1; i >= 1; i--) emit_ccx(qc, q[i + 1], anc[i - 1], anc[i]);
        emit_ccx(qc, q[0], q[1], anc[0]);
    }
    return 0;
}


/*
 * emit_diffuser — Append the Grover diffuser (reflection about the uniform state, up to a global phase of -1) on the item qubits.
 * Inputs : qc; item, N; anc, n_anc: clean |0> ancillas for the multi-controlled Z (N-3 used when N >= 5)
 * Outputs: 0 and qc extended; -1 if the multi-controlled Z could not be emitted (see emit_cnz)
 */
static int emit_diffuser(QkCircuit *qc, const uint32_t *item, int N, const uint32_t *anc, int n_anc)
{
    for (int i = 0; i < N; i++) emit_h(qc, item[i]);
    for (int i = 0; i < N; i++) emit_x(qc, item[i]);
    int rc = emit_cnz(qc, item, N, anc, n_anc);
    for (int i = 0; i < N; i++) emit_x(qc, item[i]);
    for (int i = 0; i < N; i++) emit_h(qc, item[i]);
    return rc;
}


/* ── Deterministic PRNG (xorshift64*, Vigna 2016; ALGORITHM_REFERENCES.pdf item 15), seeded for reproducible tests. ── */
typedef struct { uint64_t s; } grng;

/*
 * grng_seed — Seed the solver's random generator.
 * Inputs : r; seed
 * Outputs: r initialised
 */
static void grng_seed(grng *r, uint64_t seed)
{
    r->s = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

/*
 * grng_u64 — Next 64-bit pseudo-random value.
 * Inputs : r
 * Outputs: value
 */
static uint64_t grng_u64(grng *r)
{
    uint64_t x = r->s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/*
 * grng_unit — Uniform double in [0, 1).
 * Inputs : r
 * Outputs: value
 */
static double grng_unit(grng *r)               /* uniform double in [0,1) */
{
    return (double)(grng_u64(r) >> 11) * (1.0 / 9007199254740992.0);
}

/*
 * grng_below — Uniform integer in [0, n).
 * Inputs : r; n
 * Outputs: value
 */
static uint32_t grng_below(grng *r, uint32_t n)  /* uniform integer in [0,n) */
{
    return n ? (uint32_t)(grng_u64(r) % n) : 0u;
}

/* ── Layout + instance context shared by the solver routines. ────────────────── */
typedef struct {
    int            N;                  /* number of items                          */
    const uint32_t *costs, *values;    /* classical item costs / values            */
    uint32_t       W;                  /* budget                                   */
    uint32_t       m_cost, m_val, m_carry;
    int            NQ;                  /* total qubits                             */
    uint32_t       item[32], cost_q[32], value_q[32], carry_q[32], feas, valok;
} gctx;

/* ── Classical scoring of a subset bitmask (bit i set ⇒ item i chosen). ───────── */

/*
 * subset_cost — Total cost of the subset with bitmask x.
 * Inputs : x: bitmask, bit i = item i; g: problem context
 * Outputs: cost
 */
static uint32_t subset_cost(uint32_t x, const gctx *g)
{
    uint32_t c = 0;
    for (int i = 0; i < g->N; i++) if ((x >> i) & 1u) c += g->costs[i];
    return c;
}

/*
 * subset_value — Total value of the subset with bitmask x.
 * Inputs : x; g
 * Outputs: value
 */
static uint32_t subset_value(uint32_t x, const gctx *g)
{
    uint32_t v = 0;
    for (int i = 0; i < g->N; i++) if ((x >> i) & 1u) v += g->values[i];
    return v;
}

/*
 * is_marked — Whether subset x fits the budget and reaches the threshold T.
 * Inputs : x; g; T
 * Outputs: 1 or 0
 */
static int is_marked(uint32_t x, const gctx *g, uint32_t T)   /* feasible ∧ value≥T */
{
    return (subset_cost(x, g) <= g->W) && (subset_value(x, g) >= T);
}

/* smallest m with 2^m > sum, so the accumulator never overflows its top bit. */

/*
 * width_for_sum — Smallest register width m with 2^m > sum.
 * Inputs : sum
 * Outputs: m
 */
static uint32_t width_for_sum(uint32_t sum)
{
    uint32_t m = 1;
    while (((uint32_t)1 << m) <= sum) m++;
    return m;
}


/*
 * setup_gctx — Lay out the qubit registers (items, cost, value, carry, two flags) for a problem.
 * Inputs : g: context to fill; N, costs, values, W
 * Outputs: g filled, including the total qubit count NQ
 */
static void setup_gctx(gctx *g, int N, const uint32_t *costs,
                       const uint32_t *values, uint32_t W)
{
    g->N = N; g->costs = costs; g->values = values; g->W = W;

    uint32_t csum = 0, vsum = 0;
    for (int i = 0; i < N; i++) { csum += costs[i]; vsum += values[i]; }
    g->m_cost  = width_for_sum(csum);
    g->m_val   = width_for_sum(vsum);
    g->m_carry = (g->m_cost > g->m_val) ? g->m_cost : g->m_val;
    /* The diffuser's C^(N-1)Z (N >= 5) borrows N-3 clean ancillas from these registers. */
    if (N >= 5) {
        uint32_t pool = g->m_cost + g->m_val + g->m_carry, need = (uint32_t)(N - 3);
        if (pool < need) g->m_carry += need - pool;
    }

    uint32_t b = 0;
    for (int i = 0; i < N; i++)               g->item[i]    = b++;
    for (uint32_t i = 0; i < g->m_cost;  i++) g->cost_q[i]  = b++;
    for (uint32_t i = 0; i < g->m_val;   i++) g->value_q[i] = b++;
    for (uint32_t i = 0; i < g->m_carry; i++) g->carry_q[i] = b++;
    g->feas = b++; g->valok = b++;
    g->NQ = (int)b;
}

/* ── Configuration knobs (compile-time changeable, e.g. -DGROVER_K=5) ─────────── */
#ifndef KNAPSACK_NMAX
#define KNAPSACK_NMAX      7    /* largest N the quantum solver accepts (NQ simulable);
                                 * N > Nmax -> ExStatus -3 "too large".  Raise as the
                                 * hardware / simulation capability grows.          */
#endif
#ifndef GROVER_K
#define GROVER_K           3    /* best-of-K: return the highest-value of K quantum
                                 * solves (reliability hook for noise / hardware).  */
#endif
#define SOLVER_PATH_SIM    0
#define SOLVER_PATH_AER    1
#define SOLVER_PATH_QPU    2
#define SOLVER_QASM_FILE   "solverCircuit.qasm"
#define SOLVER_META_FILE   "solverCircuit.meta"
#define AER_RESULTS_FILE   "aer_results.json"
#define QPU_RESULTS_FILE   "qpu_results.json"
#ifndef SOLVER_WAIT_TIMEOUT_S
#define SOLVER_WAIT_TIMEOUT_S 7200 /* paths 1/2: wait at most 2 h for the results file */
#endif
#define SOLVER_POLL_SECONDS 2       /* paths 1/2: poll interval                        */
#ifndef WRITE_GROVER_STATS
#define WRITE_GROVER_STATS 1    /* callKnapsack dumps a stats file for post-processing */
#endif
#define GROVER_MAX_K       32   /* cap on K for the gstats per-run arrays            */
#define GROVER_MAX_STATES  256  /* cap on 2^N for the outcome histogram (N <= 8)     */

typedef struct {
    int      K;                               /* best-of-K runs performed            */
    uint32_t run_value[GROVER_MAX_K];         /* value each run returned             */
    long     run_oracle_calls[GROVER_MAX_K];  /* oracle invocations per run          */
    uint32_t best_value, best_mask;           /* the returned (best) answer          */
    long     total_oracle_calls;              /* across all K runs (query complexity) */
    int      total_grover_samples;            /* total circuit builds + measurements */
    int      total_dh_rounds;                 /* Durr-Hoyer grover_search calls       */
    long     measure_hist[GROVER_MAX_STATES]; /* histogram of measured item-states    */
} gstats;

static void gstats_reset(gstats *st) { if (st) memset(st, 0, sizeof *st); }


/*
 * build_grover_circuit — Build the circuit: Hadamards on the items, then j repetitions of oracle + diffuser.
 * Inputs : g; T: value threshold; j: Grover iterations
 * Outputs: new QkCircuit (caller frees), or NULL (message on stderr) if the diffuser could not be emitted
 */
static QkCircuit *build_grover_circuit(const gctx *g, uint32_t T, int j)
{
    /* the oracle's registers are |0> between oracle and diffuser: the diffuser's ancilla pool */
    uint32_t anc[96]; int n_anc = 0;
    for (uint32_t i = 0; i < g->m_cost;  i++) anc[n_anc++] = g->cost_q[i];
    for (uint32_t i = 0; i < g->m_val;   i++) anc[n_anc++] = g->value_q[i];
    for (uint32_t i = 0; i < g->m_carry; i++) anc[n_anc++] = g->carry_q[i];

    QkCircuit *qc = qk_circuit_new(g->NQ, 0);
    for (int i = 0; i < g->N; i++) emit_h(qc, g->item[i]);          /* prepare |s>  */
    for (int it = 0; it < j; it++) {
        emit_oracle(qc, g->item, g->N, g->costs, g->values,
                    g->cost_q, g->m_cost, g->value_q, g->m_val, g->carry_q,
                    g->feas, g->valok, g->W, T);
        if (emit_diffuser(qc, g->item, g->N, anc, n_anc) != 0) {
            fprintf(stderr, "build_grover_circuit: no diffuser for N=%d — circuit discarded\n", g->N);
            qk_circuit_free(qc);
            return NULL;
        }
    }
    return qc;
}


/*
 * run_and_measure — Simulate the circuit and sample one measurement of the item register.
 * Inputs : qc; g; r: random generator
 * Outputs: measured subset bitmask
 */
static uint32_t run_and_measure(const QkCircuit *qc, const gctx *g, grng *r)
{
    double complex *sv = sv_alloc(g->NQ);
    simulate_circuit(qc, sv, g->NQ);

    size_t item_states = (size_t)1 << g->N;
    double *p = (double *)calloc(item_states, sizeof(double));
    double total = 0.0;
    for (size_t s = 0; s < ((size_t)1 << g->NQ); s++) {
        double pr = creal(sv[s]) * creal(sv[s]) + cimag(sv[s]) * cimag(sv[s]);
        p[s & (item_states - 1)] += pr;            /* low N bits = item register   */
        total += pr;
    }

    double u = grng_unit(r) * total, acc = 0.0;
    uint32_t x = (uint32_t)(item_states - 1);
    for (uint32_t k = 0; k < item_states; k++) { acc += p[k]; if (u < acc) { x = k; break; } }

    free(p);
    free(sv);
    return x;
}


/*
 * grover_sample — Build, run and measure one Grover circuit with j iterations; account for the oracle calls.
 * Inputs : g; T; j; r; st: statistics (may be NULL)
 * Outputs: measured subset bitmask, or UINT32_MAX if the circuit could not be built
 */
static uint32_t grover_sample(const gctx *g, uint32_t T, int j, grng *r, gstats *st)
{
    QkCircuit *qc = build_grover_circuit(g, T, j);
    if (!qc) return UINT32_MAX;                   /* no circuit: no sample (never marked) */
    uint32_t   x  = run_and_measure(qc, g, r);
    qk_circuit_free(qc);

    if (st) {
        st->total_oracle_calls   += j;     /* j oracle invocations in this circuit  */
        st->total_grover_samples += 1;
        if (x < (uint32_t)GROVER_MAX_STATES) st->measure_hist[x] += 1;
    }
    return x;
}


/* Why two nested loops.
 * Grover's iteration count depends on M, the number of subsets that pass the check, and M is
 * not known in advance. Boyer, Brassard, Høyer and Tapp (1998) showed that drawing the count
 * at random below a bound that grows geometrically finds a passing subset in about sqrt(2^N / M)
 * oracle calls without ever learning M; that is grover_search. Dürr and Høyer (1996) turn
 * "find any subset with value >= T" into "find the best subset" by raising T to each value found
 * and searching again until nothing passes; that is durr_hoyer. Every sample is checked
 * classically before it is accepted, so a wrong measurement can only cost time, never
 * correctness of the accepted answer. */

/*
 * grover_search — Search for a subset that fits the budget and reaches T, with the iteration count drawn as in Boyer et al. (1998).
 * Inputs : g; T; r; st
 * Outputs: 1 and *out_x = the subset found, or 0 if none within the budget
 */
static int grover_search(const gctx *g, uint32_t T, grng *r, gstats *st, uint32_t *out_x)
{
    size_t NN     = (size_t)1 << g->N;
    double mmax   = sqrt((double)NN);
    double lambda = 1.2;                 /* 6/5, in (1, 4/3) per BBHT              */
    double m      = 1.0;

    /* rounds for m to climb from 1 to √NN (= ⌈log_λ √NN⌉), plus a margin so that a
     * genuinely empty marked set (M=0) is distinguished from bad luck reliably. */
    int grow_steps = (mmax > 1.0) ? (int)ceil(log(mmax) / log(lambda)) : 0;
    int max_rounds = grow_steps + 8;   /* best-of-K (GROVER_K) is the outer net */

    for (int round = 0; round < max_rounds; round++) {
        uint32_t cap = (uint32_t)ceil(m);
        int j = (int)grng_below(r, cap > 0 ? cap : 1);   /* random j ∈ [0,⌈m⌉)     */
        uint32_t x = grover_sample(g, T, j, r, st);
        if (x == UINT32_MAX) return 0;                    /* circuit not built: give up */
        if (is_marked(x, g, T)) { *out_x = x; return 1; } /* accept only real hits  */
        m *= lambda; if (m > mmax) m = mmax;
    }
    return 0;
}


/*
 * durr_hoyer — Maximum finding: raise the threshold to each value found until no better subset is found (Durr and Hoyer, 1996; this version starts from the empty set rather than a random index).
 * Inputs : g; r; st
 * Outputs: *out_mask, *out_value: the best subset found
 */
static void durr_hoyer(const gctx *g, grng *r, gstats *st, uint32_t *out_mask, uint32_t *out_value)
{
    uint32_t best_mask  = 0;                       /* ∅ : cost 0 ≤ W, always feasible */
    uint32_t best_value = subset_value(0, g);      /* = 0                              */
    uint32_t T          = best_value + 1;

    for (;;) {
        uint32_t x;
        if (st) st->total_dh_rounds++;
        if (!grover_search(g, T, r, st, &x)) break;    /* no feasible value ≥ T → done    */
        uint32_t v = subset_value(x, g);
        if (v > best_value) { best_value = v; best_mask = x; T = v + 1; }
        else break;                                /* guard (v ≥ T > best can't fail)  */
    }
    *out_mask  = best_mask;
    *out_value = best_value;
}


/*
 * grover_knapsack — Top-level quantum solve: lay out the registers and run the search K times, keeping the best answer.
 * Inputs : N, costs, values, W; seed; K: number of runs; st
 * Outputs: *out_mask, *out_value
 */
static void grover_knapsack(int N, const uint32_t *costs, const uint32_t *values,
                            uint32_t W, uint64_t seed, int K, gstats *st,
                            uint32_t *out_mask, uint32_t *out_value)
{
    gctx g;
    setup_gctx(&g, N, costs, values, W);

    gstats_reset(st);
    if (K < 1) K = 1;
    if (K > GROVER_MAX_K) K = GROVER_MAX_K;

    uint32_t best_mask = 0, best_value = 0;
    for (int k = 0; k < K; k++) {
        long  before = st ? st->total_oracle_calls : 0;
        grng  r;  grng_seed(&r, seed + (uint64_t)k * 0x9E3779B97F4A7C15ULL);
        uint32_t mask, value;
        durr_hoyer(&g, &r, st, &mask, &value);
        if (value > best_value) { best_value = value; best_mask = mask; }
        if (st) { st->run_value[k] = value; st->run_oracle_calls[k] = st->total_oracle_calls - before; }
    }

    if (st) { st->K = K; st->best_value = best_value; st->best_mask = best_mask; }
    *out_mask = best_mask; *out_value = best_value;
}

static int write_qasm(FILE *f, const QkCircuit *qc);   /* OpenQASM-3 exporter */

/* ── The solver-path interface: every path takes the same problem and returns the same
 *    result through the same exit, so solve_knapsack() and everything above it cannot
 *    tell the paths apart.  Status codes (returned by every solver_*()):            */
#define KP_OK            0
#define KP_NO_FEASIBLE (-1)   /* only the empty set fits the budget                    */
#define KP_ERROR       (-2)   /* bad path, file I/O, malformed or mismatched results  */
#define KP_TIMEOUT     (-6)   /* an out-of-process path did not answer in time        */

typedef struct {              /* the problem, after the model's doubles are converted */
    int      N;
    uint32_t costs[MAX_ITEMS], values[MAX_ITEMS];
    uint32_t csum, vsum, W;
    uint64_t seed;            /* deterministic per instance (path 0)                  */
} kp_problem;

typedef struct {              /* the standard result every path returns                */
    uint32_t qmask, qvalue;   /* the selected subset and its value                     */
    int      verification;    /* 1 = phase-1 verification run (threshold from the
                               *     classical check), 0 = the adaptive quantum search  */
    long     shots;           /* paths 1/2: shots measured                              */
    double   p_success;       /* paths 1/2: fraction of shots on the marked (optimal) set */
    double   p_random;        /* paths 1/2: the chance baseline M / 2^N                 */
    long     oracle_calls;    /* path 0: oracle invocations in the search; 0 for paths 1/2 */
} kp_result;


/* What OpenQASM is, and why the out-of-process paths depend on it.
 *
 * OpenQASM (Open Quantum Assembly Language) is a plain-text format for describing a
 * quantum circuit: a version line, an include of the standard gate library
 * ("stdgates.inc": h, x, cx, ccx, ...), a qubit register declaration, and then one
 * statement per gate in execution order.  It is the common interchange format of the
 * quantum software tools; Qiskit reads it with qiskit.qasm3.loads().
 *
 * Paths 1 (Aer) and 2 (QPU) cannot run the circuit inside this process, so the program
 * writes the circuit it built as OpenQASM 3 (write_qasm, later in this file) together
 * with a sidecar .meta file naming the qubits to measure and the marked outcomes.  An
 * external runner, which this repository does not include, loads the .qasm file,
 * executes it on Aer or on an IBM processor, and writes the measured counts back as
 * JSON; solver_external() below reads them.  The OpenQASM text is the only thing that
 * crosses the process boundary: this C code never calls Qiskit's Python side.
 *
 * Documentation: the OpenQASM Live Specification, https://openqasm.com/ (standard gate
 * library: https://openqasm.com/language/standard_library.html); source and issues:
 * https://github.com/openqasm/openqasm.  Paper: A. W. Cross et al., "OpenQASM 3: A
 * Broader and Deeper Quantum Assembly Language," ACM Trans. Quantum Comput. 3(3)
 * (2022), arXiv:2104.14722.  See ALGORITHM_REFERENCES.pdf item 16.                     */

/*
 * export_solver_circuit — Write the verification circuit (threshold = the classical optimum) as OpenQASM 3 plus a metadata file for an external runner.
 * Inputs : path: 1 Aer, 2 QPU; p: problem; request_id; marked: array to receive the marked subsets
 * Outputs: KP_OK or an error code; *M_out = number of marked subsets
 */
static int export_solver_circuit(int path, const kp_problem *p, unsigned long long request_id,
                                 unsigned char *marked, uint32_t *M_out)
{
    const int N = p->N;
    gctx g;
    setup_gctx(&g, N, p->costs, p->values, p->W);

    uint32_t copt = 0;                            /* classical optimum (ground truth) */
    for (uint32_t x = 0; x < (1u << N); x++) {
        uint32_t c = 0, v = 0;
        for (int i = 0; i < N; i++) if ((x >> i) & 1u) { c += p->costs[i]; v += p->values[i]; }
        if (c <= p->W && v > copt) copt = v;
    }
    if (copt == 0) return KP_NO_FEASIBLE;         /* only the empty set fits */

    uint32_t T = copt, M = 0;
    for (uint32_t x = 0; x < (1u << N); x++) {
        marked[x] = (unsigned char)is_marked(x, &g, T);
        M += marked[x];
    }
    double theta = asin(sqrt((double)M / (double)(1u << N)));
    int j = (int)floor(M_PI / (4.0 * theta));
    if (j < 1) j = 1;

    QkCircuit *qc = build_grover_circuit(&g, T, j);
    if (!qc) return KP_ERROR;
    int rc = KP_ERROR;
    FILE *f = fopen(SOLVER_QASM_FILE, "w");
    if (f) { rc = write_qasm(f, qc) == 0 ? KP_OK : KP_ERROR; fclose(f); }
    qk_circuit_free(qc);
    if (rc != KP_OK) { fprintf(stderr, "solve_knapsack: could not write %s\n", SOLVER_QASM_FILE); return KP_ERROR; }

    FILE *m = fopen(SOLVER_META_FILE, "w");
    if (!m) { fprintf(stderr, "solve_knapsack: could not write %s\n", SOLVER_META_FILE); return KP_ERROR; }
    fprintf(m, "N %d\nNQ %d\nW %u\nT %u\nj %d\npath %d\nrequest_id %llu\n",
            g.N, g.NQ, p->W, T, j, path, request_id);
    fprintf(m, "item_qubits");
    for (int i = 0; i < g.N; i++) fprintf(m, " %u", g.item[i]);
    fprintf(m, "\nmarked_masks");
    for (uint32_t x = 0; x < (1u << N); x++) if (marked[x]) fprintf(m, " %u", x);
    fprintf(m, "\n");
    fclose(m);

    printf("  +-- Quantum Grover Knapsack Solver: path %d (%s) -----\n", path,
           path == SOLVER_PATH_AER ? "Aer noise model" : "IBM QPU");
    printf("  |  Items N             : %d   (Nmax = %d)\n", N, KNAPSACK_NMAX);
    printf("  |  Circuit qubits NQ   : %d\n", g.NQ);
    printf("  |  VERIFICATION run    : threshold T = %u (optimum, from the classical check), %u marked, j = %d\n", T, M, j);
    printf("  |  Exported            : %s + %s  (request_id %llu)\n", SOLVER_QASM_FILE, SOLVER_META_FILE, request_id);
    *M_out = M;
    return KP_OK;
}


/*
 * json_key — Locate a key in a JSON text.
 * Inputs : buf; key
 * Outputs: pointer past the colon, or NULL
 */
static const char *json_key(const char *buf, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *q = strstr(buf, pat);
    if (!q) return NULL;
    q = strchr(q + strlen(pat), ':');
    return q ? q + 1 : NULL;
}

/*
 * json_ull — Read an unsigned integer value by key.
 * Inputs : buf; key
 * Outputs: 0 on success; *out
 */
static int json_ull(const char *buf, const char *key, unsigned long long *out)
{
    const char *q = json_key(buf, key);
    if (!q) return -1;
    char *end;
    *out = strtoull(q, &end, 10);
    return (end == q) ? -1 : 0;
}

/*
 * json_str — Read a string value by key.
 * Inputs : buf; key; out, n
 * Outputs: 0 on success
 */
static int json_str(const char *buf, const char *key, char *out, size_t n)
{
    const char *q = json_key(buf, key);
    if (!q || !(q = strchr(q, '"'))) return -1;
    const char *e = strchr(++q, '"');
    if (!e || (size_t)(e - q) >= n) return -1;
    memcpy(out, q, (size_t)(e - q)); out[e - q] = '\0';
    return 0;
}

/*
 * json_array — Read an integer array by key.
 * Inputs : buf; key; out, len
 * Outputs: number of values read, or -1
 */
static long json_array(const char *buf, const char *key, long *out, long len)
{
    const char *q = json_key(buf, key);
    if (!q || !(q = strchr(q, '['))) return -1;
    q++;
    long k = 0;
    for (;;) {
        while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
        if (*q == ']') return k;
        if (k == len) return -1;
        char *end;
        out[k++] = strtol(q, &end, 10);
        if (end == q) return -1;
        q = end;
        while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
        if (*q == ',') q++;
    }
}

/*
 * wait_for_results — Poll for the results file until it appears or the timeout elapses.
 * Inputs : results_file; timeout_s
 * Outputs: file contents (caller frees), or NULL on timeout
 */
static char *wait_for_results(const char *results_file, long timeout_s)
{
    time_t t0 = time(NULL);
    for (;;) {
        FILE *f = fopen(results_file, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *buf = (n > 0) ? malloc((size_t)n + 1) : NULL;
            size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
            fclose(f);
            if (buf && got == (size_t)n) { buf[n] = '\0'; return buf; }
            free(buf);
            return NULL;
        }
        if ((long)difftime(time(NULL), t0) >= timeout_s) return NULL;
        struct timespec ts = { SOLVER_POLL_SECONDS, 0 };
        thrd_sleep(&ts, NULL);
    }
}


/*
 * solver_external — The out-of-process protocol shared by paths 1 and 2: export, wait, validate, convert the counts to the standard result.
 * Inputs : path; results_file; p
 * Outputs: KP_OK / KP_NO_FEASIBLE / KP_ERROR / KP_TIMEOUT; r filled; SolutionProbs = share of shots per subset
 */
static int solver_external(int path, const char *results_file, const kp_problem *p, kp_result *r,
                           double *SolutionProbs)
{
    static unsigned counter = 0;
    const int N = p->N;
    const uint32_t S = 1u << N;
    unsigned long long request_id = ((unsigned long long)time(NULL) << 16) | (counter++ & 0xFFFFu);

    unsigned char *marked = calloc(S, 1);
    long *counts = calloc(S, sizeof(long));
    if (!marked || !counts) { free(marked); free(counts); return KP_ERROR; }

    remove(results_file);                         /* never read a stale answer */
    uint32_t M = 0;
    int rc = export_solver_circuit(path, p, request_id, marked, &M);
    if (rc != KP_OK) { free(marked); free(counts); return rc; }

    long timeout_s = SOLVER_WAIT_TIMEOUT_S;
    const char *env = getenv("KNAPSACK_WAIT_TIMEOUT_S");
    if (env && atol(env) > 0) timeout_s = atol(env);
    printf("  |  Waiting for         : %s  (poll %d s, timeout %ld s)\n",
           results_file, SOLVER_POLL_SECONDS, timeout_s);
    fflush(stdout);

    char *buf = wait_for_results(results_file, timeout_s);
    if (!buf) {
        fprintf(stderr, "solve_knapsack: no %s within %ld s (path %d timed out)\n",
                results_file, timeout_s, path);
        printf("  +-- TIMED OUT ------------------------------------------------\n\n");
        free(marked); free(counts);
        return KP_TIMEOUT;
    }

    unsigned long long rid = 0, rpath = 0, shots = 0;
    char backend[64] = "?", job_id[64] = "?";
    long ncount = -1;
    if (json_ull(buf, "request_id", &rid) || json_ull(buf, "path", &rpath) ||
        json_ull(buf, "shots", &shots) ||
        (ncount = json_array(buf, "counts_by_mask", counts, (long)S)) < 0) {
        fprintf(stderr, "solve_knapsack: %s is malformed\n", results_file);
        free(buf); free(marked); free(counts);
        return KP_ERROR;
    }
    json_str(buf, "backend", backend, sizeof backend);
    json_str(buf, "job_id", job_id, sizeof job_id);
    free(buf);

    long total = 0;
    for (uint32_t x = 0; x < S; x++) total += counts[x];
    if (rid != request_id || rpath != (unsigned long long)path || ncount != (long)S ||
        shots == 0 || total != (long)shots) {
        fprintf(stderr, "solve_knapsack: %s does not match this request "
                "(request_id %llu vs %llu, path %llu vs %d, %ld counts vs %u, sum %ld vs shots %llu)\n",
                results_file, rid, request_id, rpath, path, ncount, S, total, shots);
        free(marked); free(counts);
        return KP_ERROR;
    }

    /* Standard form: the most frequent feasible nonempty measured subset. */
    uint32_t best = 0; long best_n = 0, hits = 0;
    for (uint32_t x = 0; x < S; x++) {
        if (marked[x]) hits += counts[x];
        uint32_t c = 0;
        for (int i = 0; i < N; i++) if ((x >> i) & 1u) c += p->costs[i];
        if (x != 0 && c <= p->W && counts[x] > best_n) { best = x; best_n = counts[x]; }
    }
    uint32_t v = 0;
    for (int i = 0; i < N; i++) if ((best >> i) & 1u) v += p->values[i];

    r->qmask = best; r->qvalue = v; r->verification = 1;
    r->shots = (long)shots;
    r->oracle_calls = 0;
    if (SolutionProbs)                                   /* the standard distribution: share of shots per subset */
        for (uint32_t x = 0; x < S; x++) SolutionProbs[x] = (double)counts[x] / (double)shots;
    r->p_success = (double)hits / (double)shots;
    r->p_random  = (double)M / (double)S;

    printf("  |  Results             : %s  backend %s  job %s\n", results_file, backend, job_id);
    printf("  |  P(success)          : %.4f on %ld shots   (chance %.4f)\n",
           r->p_success, r->shots, r->p_random);
    printf("  |  Selected subset     : mask 0x%X, value %u   (most frequent feasible outcome, %ld shots)\n",
           best, v, best_n);
    printf("  +----------------------------------------------------------\n\n");
    free(marked); free(counts);
    return (best == 0) ? KP_NO_FEASIBLE : KP_OK;
}


/*
 * solver_sim — SolverPath 0: the in-process quantum solve on the pure-C simulator.
 * Inputs : p: problem
 * Outputs: return code; r: subset, value, oracle calls; SolutionProbs: exact outcome distribution of the final circuit (simulated exactly; not counted in oracle_calls)
 */
static int solver_sim(const kp_problem *p, kp_result *r, double *SolutionProbs)
{
    const int       N        = p->N;
    const uint32_t *costs_u  = p->costs, *values_u = p->values;
    const uint32_t  csum     = p->csum,  vsum = p->vsum, W = p->W;
    const uint64_t  seed     = p->seed;

    gstats   st;
    uint32_t qmask = 0, qvalue = 0;
    grover_knapsack(N, costs_u, values_u, W, seed, GROVER_K, &st, &qmask, &qvalue);

    /* ── Independent classical CHECKER (ground truth only — never seeds the oracle,
     *    never overrides the quantum answer; logs loudly on disagreement). ──────── */
    uint32_t copt = 0;
    for (uint32_t x = 0; x < (1u << N); x++) {
        uint32_t c = 0, v = 0;
        for (int i = 0; i < N; i++) if ((x >> i) & 1u) { c += costs_u[i]; v += values_u[i]; }
        if (c <= W && v > copt) copt = v;
    }
    if (qvalue != copt)
        fprintf(stderr, "solve_knapsack: WARNING quantum value %u != classical optimum %u "
                        "(rare bad Grover run; returning the quantum result per "
                        "pure-quantum policy)\n", qvalue, copt);

    /* ── Honest summary to stdout (the circuit is now the Grover circuit). ──────── */
    uint32_t mc = width_for_sum(csum), mv = width_for_sum(vsum);
    int NQ = N + (int)mc + (int)mv + (int)((mc > mv) ? mc : mv) + 2;
    printf("  +-- Quantum Grover Knapsack Solver (BBHT + Durr-Hoyer) -----\n");
    printf("  |  Items N             : %d   (Nmax = %d)\n", N, KNAPSACK_NMAX);
    printf("  |  Circuit qubits NQ   : %d   (item+cost+value+carry+2 flags)\n", NQ);
    printf("  |  Best-of-K runs      : %d\n", st.K);
    printf("  |  Oracle calls total  : %ld   (Grover query-complexity metric)\n", st.total_oracle_calls);
    r->oracle_calls = st.total_oracle_calls;
    printf("  |  Quantum optimum     : value %u   (subset mask 0x%X)\n", qvalue, qmask);
    printf("  |  Classical check     : %s (optimum %u)\n",
           (qvalue == copt) ? "AGREES" : "DISAGREES", copt);
    {
        size_t num_states = (size_t)1 << N;
        for (size_t s = 0; s < num_states; s++) SolutionProbs[s] = 0.0;
        if (qvalue > 0) {
            gctx g;
            setup_gctx(&g, N, costs_u, values_u, W);
            uint32_t M = 0;
            for (uint32_t x = 0; x < (uint32_t)num_states; x++) M += (uint32_t)is_marked(x, &g, qvalue);
            if (M > 0) {
                double theta = asin(sqrt((double)M / (double)num_states));
                int j = (int)floor(M_PI / (4.0 * theta));
                if (j < 1) j = 1;
                QkCircuit *qc = build_grover_circuit(&g, qvalue, j);
                if (qc) {
                    double complex *sv = sv_alloc(g.NQ);
                    simulate_circuit(qc, sv, g.NQ);
                    for (size_t s = 0; s < ((size_t)1 << g.NQ); s++) {
                        double pr = creal(sv[s]) * creal(sv[s]) + cimag(sv[s]) * cimag(sv[s]);
                        SolutionProbs[s & (num_states - 1)] += pr;      /* low N bits = item register */
                    }
                    free(sv);
                    qk_circuit_free(qc);
                    printf("  |  Final circuit       : T = %u, %u marked, j = %d   (exact outcome distribution reported to the caller)\n",
                           qvalue, M, j);
                }
            }
        }
    }
    printf("  +----------------------------------------------------------\n\n");

    /* ── Optional statistics file for post-processing (compile-time toggle). ────── */
#if WRITE_GROVER_STATS
    {
        FILE *sf = fopen(STATS_OUTPUT_FILE, "w");
        if (sf) {
            fprintf(sf, "# Grover knapsack solver statistics\n");
            fprintf(sf, "N=%d Nmax=%d W=%u K=%d NQ=%d\n", N, KNAPSACK_NMAX, W, st.K, NQ);
            fprintf(sf, "best_value=%u best_mask=0x%X classical_optimum=%u match=%d\n",
                    st.best_value, st.best_mask, copt, (qvalue == copt));
            fprintf(sf, "total_oracle_calls=%ld total_grover_samples=%d total_dh_rounds=%d\n",
                    st.total_oracle_calls, st.total_grover_samples, st.total_dh_rounds);
            for (int k = 0; k < st.K; k++)
                fprintf(sf, "run %d: value=%u oracle_calls=%ld\n",
                        k, st.run_value[k], st.run_oracle_calls[k]);
            fprintf(sf, "# measured-outcome histogram (item-state : count)\n");
            for (uint32_t s = 0; s < (1u << N); s++)
                fprintf(sf, "0x%X %ld\n", s,
                        (s < (uint32_t)GROVER_MAX_STATES) ? st.measure_hist[s] : 0L);
            fclose(sf);
            printf("  Grover statistics written to: %s\n\n", STATS_OUTPUT_FILE);
        }
    }
#endif

    r->qmask = qmask; r->qvalue = qvalue; r->verification = 0;
    return KP_OK;
}


/*
 * solver_aer — SolverPath 1: the Aer noise-model simulation, run out of process.
 * Inputs : p
 * Outputs: as solver_external
 */
static int solver_aer(const kp_problem *p, kp_result *r, double *SolutionProbs)
{
    return solver_external(SOLVER_PATH_AER, AER_RESULTS_FILE, p, r, SolutionProbs);
}


/*
 * solver_qpu — SolverPath 2: an IBM quantum processor, run out of process.
 * Inputs : p
 * Outputs: as solver_external
 */
static int solver_qpu(const kp_problem *p, kp_result *r, double *SolutionProbs)
{
    return solver_external(SOLVER_PATH_QPU, QPU_RESULTS_FILE, p, r, SolutionProbs);
}


/*
 * solve_knapsack — Select the solver path, run it, and map the standard result to the caller's arrays.
 * Inputs : N, Costs, Values, CostLimit; SolverPath; SolutionProbs: caller-allocated 2^N doubles
 * Outputs: best value found (negative sentinels: -1 none feasible, -2 error, -3 too large, -6 timeout); PatentIndices, out_count: the selected items; out_verification, out_res
 */
static double solve_knapsack(
    int            N,
    const double  *Costs,
    const double  *Values,
    double         CostLimit,
    int           *PatentIndices,   /* caller-allocated int[N] — selected indices */
    int           *out_count,
    double        *SolutionProbs,
    int            SolverPath,      /* SOLVER_PATH_SIM / _AER / _QPU */
    int           *out_verification,/* OUT: 1 = answer from a VERIFICATION run (paths 1/2) */
    kp_result     *out_res          /* OUT (may be NULL): the standard result for the caller */
) {
    *out_verification = 0;
    if (N <= 0 || N > MAX_ITEMS) {
        fprintf(stderr, "solve_knapsack: N=%d out of range [1, %d]\n", N, MAX_ITEMS);
        return -1.0;
    }

    if (N > KNAPSACK_NMAX) {
        fprintf(stderr, "solve_knapsack: N=%d exceeds quantum solver limit Nmax=%d "
                        "(problem too large)\n", N, KNAPSACK_NMAX);
        *out_count = 0;
        return -3.0;
    }

    /* ── Convert the model's small-integer doubles to the solver's uint32 ───────── */
    uint32_t costs_u[MAX_ITEMS], values_u[MAX_ITEMS], csum = 0, vsum = 0;
    for (int i = 0; i < N; i++) {
        costs_u[i]  = (Costs[i]  > 0.0) ? (uint32_t)(Costs[i]  + 0.5) : 0u;
        values_u[i] = (Values[i] > 0.0) ? (uint32_t)(Values[i] + 0.5) : 0u;
        csum += costs_u[i];
        vsum += values_u[i];
    }
    /* Budget: clamp to [0, Sum cost].  A budget >= total cost means "all feasible",
     * and clamping keeps W inside the cost register's range that the comparator needs. */
    uint32_t W = (CostLimit < 0.0) ? 0u : (uint32_t)(CostLimit + 0.5);
    if (W > csum) W = csum;

    /* ── Deterministic per-instance seed (reproducible OpenModelica runs) ───────── */
    uint64_t seed = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)N << 1) ^ ((uint64_t)W << 8);
    for (int i = 0; i < N; i++) {
        seed = (seed ^ costs_u[i])  * 0x100000001B3ULL;
        seed = (seed ^ values_u[i]) * 0x100000001B3ULL;
    }

    /* ── The problem, in the form every solver path takes. ─────────────────────── */
    kp_problem prob;
    prob.N = N; prob.csum = csum; prob.vsum = vsum; prob.W = W; prob.seed = seed;
    for (int i = 0; i < N; i++) { prob.costs[i] = costs_u[i]; prob.values[i] = values_u[i]; }
    kp_result res = {0};

    /* ── Solver-path selection: one branch per path; add a path = add a branch. ── */
    int rc;
    if (SolverPath == SOLVER_PATH_SIM) {
        rc = solver_sim(&prob, &res, SolutionProbs);
    } else if (SolverPath == SOLVER_PATH_AER) {
        rc = solver_aer(&prob, &res, SolutionProbs);
    } else if (SolverPath == SOLVER_PATH_QPU) {
        rc = solver_qpu(&prob, &res, SolutionProbs);
    } else {
        fprintf(stderr, "solve_knapsack: SolverPath=%d is not a solver path [0, 2]\n", SolverPath);
        rc = KP_ERROR;
    }

    /* ── Common exit: map the standard result to the output contract. ─────────── */
    *out_count = 0;
    if (rc == KP_TIMEOUT) return -6.0;
    if (rc == KP_ERROR)   return -2.0;
    uint32_t qmask = res.qmask, qvalue = res.qvalue;
    if (rc == KP_NO_FEASIBLE || qmask == 0 || qvalue == 0) {   /* only the empty set fit */
        return -1.0;
    }

    int  cnt = 0;
    bool seen[MAX_ITEMS];
    memset(seen, 0, (size_t)N * sizeof(bool));
    for (int i = 0; i < N; i++) {
        if ((qmask >> i) & 1u) {
            if (!seen[i]) { seen[i] = true; PatentIndices[cnt++] = i; }
        }
    }
    *out_count = cnt;
    *out_verification = res.verification;
    if (out_res) *out_res = res;

    return (double)qvalue;
}


/*
 * write_latex_circuit — Write a quantikz (LaTeX) drawing of the N-item circuit.
 * Inputs : f: output file; N
 * Outputs: file written
 */
static void write_latex_circuit(FILE *f, int N)
{
    /* ── LaTeX document preamble ─────────────────────────────────────── */
    fprintf(f,
        "\\documentclass[border=4mm]{standalone}\n"
        "\\usepackage{tikz}\n"
        "\\usepackage{quantikz}\n"
        "\n"
        "%% Quantum Knapsack Circuit — N = %d item%s\n"
        "%% Generated by callKnapsack() / write_latex_circuit() in knapsack.c\n"
        "%% Qiskit C API circuit_drawer equivalent (quantikz package)\n"
        "%%\n"
        "%% Circuit structure:\n"
        "%%   * One qubit per item (quantum register \"item\", %d qubit%s)\n"
        "%%   * One classical bit per item (classical register \"sel\", %d bit%s)\n"
        "%%   * H gate on every qubit → uniform superposition over all 2^%d subsets\n"
        "%%   * Oracle U_f then diffuser D = one Grover iteration (ancillas omitted)\n"
        "%%   * Measurement of each qubit into the corresponding classical bit\n"
        "%%\n"
        "%% Compile:  pdflatex printCircuitFile.txt\n"
        "\n"
        "\\begin{document}\n"
        "\n",
        N, (N == 1 ? "" : "s"),
        N, (N == 1 ? "" : "s"),
        N, (N == 1 ? "" : "s"),
        N
    );

    /* ── quantikz environment ─────────────────────────────────────────── */
    fprintf(f,
        "\\begin{quantikz}[row sep={0.6cm,between origins}, "
        "column sep=0.5cm]\n"
    );

    for (int i = 0; i < N; i++) {

        /* Column 1 – wire label */
        fprintf(f, "  \\lstick{$\\mathrm{item}[%d]$}", i);

        /* Column 2 – Hadamard gate */
        fprintf(f, " & \\gate{H}");

        if (i == 0)
            fprintf(f, " & \\gate[%d]{U_f} & \\gate[%d]{D}", N, N);
        else
            fprintf(f, " & & ");

        /* Column 5 – measurement into classical bit sel[i].  (quantikz \gate/\meter
         * labels are typeset in MATH mode natively, so NO $...$ — only \lstick is
         * text mode and needs the $ delimiters.) */
        fprintf(f, " & \\meter{\\mathrm{sel}[%d]}", i);

        /* End of row */
        if (i < N - 1)
            fprintf(f, " \\\\\n");
        else
            fprintf(f, "\n");   /* last row: no trailing \\ */
    }

    fprintf(f,
        "\\end{quantikz}\n"
        "\n"
        "\\end{document}\n"
    );
}


/* The two gate definitions that are not in stdgates.inc, emitted only when the circuit
 * uses them.  `ccz` is the identity H.CCX.H on the target.  `mcx` (C3X) is reproduced
 * from Qiskit's OpenQASM 2 standard library, file qiskit/qasm/libs/qelib1.inc,
 * "gate c3x a,b,c,d" (Copyright 2017 IBM and its contributors; Apache License 2.0,
 * https://www.apache.org/licenses/LICENSE-2.0), with the qubit parameters renamed
 * a,b,c,d -> c0,c1,c2,t and the statements otherwise unchanged.  `pi` is the OpenQASM 3
 * built-in constant.  See README.pdf, "Third-party notices". */
static const char *QASM_GATE_CCZ =
    "gate ccz a, b, c {\n"
    "  h c;\n"
    "  ccx a, b, c;\n"
    "  h c;\n"
    "}\n";

static const char *QASM_GATE_MCX =       /* C3X: 3 controls (c0,c1,c2) → target t */
    "gate mcx c0, c1, c2, t {\n"
    "  h t;\n"
    "  p(pi/8) c0; p(pi/8) c1; p(pi/8) c2; p(pi/8) t;\n"
    "  cx c0, c1; p(-pi/8) c1; cx c0, c1;\n"
    "  cx c1, c2; p(-pi/8) c2; cx c0, c2; p(pi/8) c2; cx c1, c2; p(-pi/8) c2; cx c0, c2;\n"
    "  cx c2, t; p(-pi/8) t; cx c1, t; p(pi/8) t; cx c2, t; p(-pi/8) t; cx c0, t; p(pi/8) t;\n"
    "  cx c2, t; p(-pi/8) t; cx c1, t; p(pi/8) t; cx c2, t; p(-pi/8) t; cx c0, t;\n"
    "  h t;\n"
    "}\n";

/*
 * qasm_param0 — First parameter of a gate instruction, in radians.
 * Inputs : inst
 * Outputs: value
 */
static double qasm_param0(const QkCircuitInstruction *inst)
{
    double p0 = 0.0;
    if (inst->num_params > 0 && inst->params && inst->params[0]) {
        char *ps = qk_param_str(inst->params[0]);
        if (ps) { p0 = strtod(ps, NULL); qk_str_free(ps); }
    }
    return p0;
}


/*
 * qasm_emit_gate — Write one circuit instruction as an OpenQASM 3 statement.
 * Inputs : f; inst
 * Outputs: 0 on success
 */
static int qasm_emit_gate(FILE *f, const QkCircuitInstruction *inst)
{
    const uint32_t *q = inst->qubits;
    const char *nm    = inst->name;

    /* single-qubit, no parameter */
    if      (!strcmp(nm, "h"))   fprintf(f, "h q[%u];\n",   q[0]);
    else if (!strcmp(nm, "x"))   fprintf(f, "x q[%u];\n",   q[0]);
    else if (!strcmp(nm, "y"))   fprintf(f, "y q[%u];\n",   q[0]);
    else if (!strcmp(nm, "z"))   fprintf(f, "z q[%u];\n",   q[0]);
    else if (!strcmp(nm, "s"))   fprintf(f, "s q[%u];\n",   q[0]);
    else if (!strcmp(nm, "sdg")) fprintf(f, "sdg q[%u];\n", q[0]);
    else if (!strcmp(nm, "t"))   fprintf(f, "t q[%u];\n",   q[0]);
    else if (!strcmp(nm, "tdg")) fprintf(f, "tdg q[%u];\n", q[0]);
    /* single-qubit, one parameter (radians, full double precision) */
    else if (!strcmp(nm, "rz"))  fprintf(f, "rz(%.17g) q[%u];\n", qasm_param0(inst), q[0]);
    else if (!strcmp(nm, "phase") || !strcmp(nm, "p"))
                                 fprintf(f, "p(%.17g) q[%u];\n",  qasm_param0(inst), q[0]);
    /* two-qubit */
    else if (!strcmp(nm, "cx"))  fprintf(f, "cx q[%u], q[%u];\n", q[0], q[1]);
    else if (!strcmp(nm, "cz"))  fprintf(f, "cz q[%u], q[%u];\n", q[0], q[1]);
    /* three-qubit (ccz uses the custom gate def above) */
    else if (!strcmp(nm, "ccx")) fprintf(f, "ccx q[%u], q[%u], q[%u];\n", q[0], q[1], q[2]);
    else if (!strcmp(nm, "ccz")) fprintf(f, "ccz q[%u], q[%u], q[%u];\n", q[0], q[1], q[2]);
    /* multi-controlled X: in this project "mcx" is always C3X (controls first,
     * target last); the custom `mcx` gate def above is 4-qubit. */
    else if (!strcmp(nm, "mcx")) {
        if (inst->num_qubits != 4) return -1;   /* only C3X is defined as `mcx` */
        fprintf(f, "mcx q[%u], q[%u], q[%u], q[%u];\n", q[0], q[1], q[2], q[3]);
    }
    else return -1;                              /* unhandled → strict guard fires */
    return 0;
}


/*
 * write_qasm — Serialise a whole QkCircuit as OpenQASM 3.  Format conventions: see the OpenQASM note before export_solver_circuit(); the two custom gate definitions (ccz, mcx) are Qiskit's, not part of the standard library.
 * Inputs : f; qc
 * Outputs: 0 on success
 */
static int write_qasm(FILE *f, const QkCircuit *qc)
{
    size_t   num_inst   = qk_circuit_num_instructions(qc);
    uint32_t num_qubits = qk_circuit_num_qubits(qc);

    /* ── Pass 1: which custom gate definitions do we need? ── */
    int uses_ccz = 0, uses_mcx = 0;
    for (size_t idx = 0; idx < num_inst; idx++) {
        if (qk_circuit_instruction_kind(qc, idx) != QkOperationKind_Gate) continue;
        QkCircuitInstruction inst;
        qk_circuit_get_instruction(qc, idx, &inst);
        if      (!strcmp(inst.name, "ccz")) uses_ccz = 1;
        else if (!strcmp(inst.name, "mcx")) uses_mcx = 1;
        qk_circuit_instruction_clear(&inst);
    }

    /* ── Preamble: version, standard library, used custom gates, qubit reg ── */
    fprintf(f, "OPENQASM 3.0;\n");
    fprintf(f, "include \"stdgates.inc\";\n");
    if (uses_ccz) fputs(QASM_GATE_CCZ, f);
    if (uses_mcx) fputs(QASM_GATE_MCX, f);
    fprintf(f, "qubit[%u] q;\n", num_qubits);

    /* ── Pass 2: body — one statement per gate, same order as the circuit ── */
    for (size_t idx = 0; idx < num_inst; idx++) {
        if (qk_circuit_instruction_kind(qc, idx) != QkOperationKind_Gate) continue;
        QkCircuitInstruction inst;
        qk_circuit_get_instruction(qc, idx, &inst);
        if (qasm_emit_gate(f, &inst) < 0) {
            fprintf(stderr,
                "write_qasm: UNHANDLED gate '%s' (%u qubit(s)) at instruction %zu — "
                "exported OpenQASM would disagree with the circuit; aborting.\n",
                inst.name, inst.num_qubits, idx);
            qk_circuit_instruction_clear(&inst);
            return -1;
        }
        qk_circuit_instruction_clear(&inst);
    }
    return 0;
}


#define QDRAW_TEXT_FILE       "circuitText.txt"
#define QDRAW_SCHEMATIC_FILE  "circuitSchematic.tex"
#define QDRAW_QASM_FILE       "circuitFigure.qasm"
#define QDRAW_QASM_FULL_FILE  "circuitFigureFull.qasm"


/*
 * write_latex_circuit_expanded — Write a register-level quantikz drawing of the circuit for the given layout.
 * Inputs : f; g
 * Outputs: file written
 */
static void write_latex_circuit_expanded(FILE *f, const gctx *g)
{
    int N  = g->N;
    int NQ = g->NQ;
    int n_cost  = (int)g->m_cost;
    int n_val   = (int)g->m_val;
    int n_carry = (int)g->m_carry;

    fprintf(f,
        "\\documentclass[border=4mm]{standalone}\n"
        "\\usepackage{tikz}\n"
        "\\usepackage{quantikz}\n"
        "\n"
        "%% Quantum Knapsack — EXPANDED schematic (QDrawDetail level 3)\n"
        "%% Generated by callKnapsack() / write_latex_circuit_expanded() in knapsack.c\n"
        "%% Registers: %d item, %d cost, %d value, %d carry, 2 flag = %d qubits.\n"
        "%% U_f is ONE Grover iteration's pipeline; it repeats under the BBHT /\n"
        "%% Durr-Hoyer schedule.  Gate-level detail: QDrawDetail 4-5 (OpenQASM).\n"
        "%% The phase-mark is CZ(feas,valok); 'uncompute' mirrors +c x / +v x / <=W / >=T.\n"
        "%% Compile:  pdflatex circuitSchematic.tex\n"
        "\n"
        "\\begin{document}\n"
        "\\begin{quantikz}[row sep={0.55cm,between origins}, column sep=0.45cm]\n",
        N, n_cost, n_val, n_carry, NQ);

    for (int row = 0; row < NQ; row++) {
        /* Column 1 — per-wire register label (ancillas start in |0>). */
        if (row < N)
            fprintf(f, "  \\lstick{$\\mathrm{item}[%d]$}", row);
        else if (row < N + n_cost)
            fprintf(f, "  \\lstick{$|0\\rangle\\,\\mathrm{cost}[%d]$}", row - N);
        else if (row < N + n_cost + n_val)
            fprintf(f, "  \\lstick{$|0\\rangle\\,\\mathrm{val}[%d]$}", row - N - n_cost);
        else if (row < N + n_cost + n_val + n_carry)
            fprintf(f, "  \\lstick{$|0\\rangle\\,\\mathrm{carry}[%d]$}",
                    row - N - n_cost - n_val);
        else if (row == NQ - 2)
            fprintf(f, "  \\lstick{$|0\\rangle\\,\\mathrm{feas}$}");
        else
            fprintf(f, "  \\lstick{$|0\\rangle\\,\\mathrm{valok}$}");

        /* Column 2 — H on item wires (prepare |s>); ancillas carry a wire. */
        fprintf(f, (row < N) ? " & \\gate{H}" : " & ");

        /* Columns 3-8 — the oracle pipeline.  Each stage is one full-register-file
         * box, carried on row 0 (item[0]); every other row leaves the cell empty so
         * the box spans all wires. */
        if (row == 0)
            fprintf(f,
                " & \\gate[%d]{+c\\cdot x} & \\gate[%d]{+v\\cdot x}"
                " & \\gate[%d]{\\le W} & \\gate[%d]{\\ge T}"
                " & \\gate[%d]{\\mathrm{mark}} & \\gate[%d]{\\mathrm{uncompute}}",
                NQ, NQ, NQ, NQ, NQ, NQ);
        else
            fprintf(f, " & & & & & & ");

        /* Column 9 — diffuser D on the item register (carried on row 0). */
        if (row == 0) fprintf(f, " & \\gate[%d]{D}", N);
        else          fprintf(f, " & ");

        /* Column 10 — measure each item wire (UNLABELED gauge); ancillas end on a
         * wire.  The classical-bit label is a separate right-stick (column 11) so the
         * sel[] text never crowds the meter symbol of the wire below it. */
        if (row < N) fprintf(f, " & \\meter{}");
        else         fprintf(f, " & ");

        /* Column 11 — classical-output label to the RIGHT of the meter.  \rstick is
         * text mode (so $...$); placing the label here (not above the meter) keeps it
         * clear of adjacent meters at tight row spacing. */
        if (row < N) fprintf(f, " & \\rstick{$\\mathrm{sel}[%d]$}", row);
        else         fprintf(f, " & ");

        fprintf(f, (row < NQ - 1) ? " \\\\\n" : "\n");
    }

    fprintf(f, "\\end{quantikz}\n\\end{document}\n");
}


/*
 * write_circuit_drawing — QDrawDetail dispatcher: write the artifact for level 1-5 (text, light, expanded, OpenQASM).
 * Inputs : level; N, Values, Costs, MaxCost
 * Outputs: artifact file written (none for level 0)
 */
static void write_circuit_drawing(int level, int N,
        const double *Values, const double *Costs, double MaxCost)
{
    if (level <= 0) return;

    /* Level 2: the standalone light schematic — no registers/circuit needed. */
    if (level == 2) {
        FILE *f = fopen(LATEX_OUTPUT_FILE, "w");
        if (!f) { perror("callKnapsack: QDrawDetail open " LATEX_OUTPUT_FILE); return; }
        write_latex_circuit(f, N);
        fclose(f);
        printf("  [QDrawDetail 2] light schematic -> %s  (pdflatex %s)\n",
               LATEX_OUTPUT_FILE, LATEX_OUTPUT_FILE);
        return;
    }

    /* Levels 1,3,4,5 resolve the instance's registers (small-integer rounding,
     * matching solve_knapsack's double -> uint32 conversion). */
    uint32_t costs_u[MAX_ITEMS], values_u[MAX_ITEMS];
    for (int i = 0; i < N; i++) {
        costs_u[i]  = (Costs[i]  > 0.0) ? (uint32_t)(Costs[i]  + 0.5) : 0u;
        values_u[i] = (Values[i] > 0.0) ? (uint32_t)(Values[i] + 0.5) : 0u;
    }
    uint32_t W = (MaxCost > 0.0) ? (uint32_t)(MaxCost + 0.5) : 0u;
    gctx g; setup_gctx(&g, N, costs_u, values_u, W);

    /* Level 3: expanded schematic — registers laid out, no circuit build needed. */
    if (level == 3) {
        FILE *f = fopen(QDRAW_SCHEMATIC_FILE, "w");
        if (!f) { perror("callKnapsack: QDrawDetail open " QDRAW_SCHEMATIC_FILE); return; }
        write_latex_circuit_expanded(f, &g);
        fclose(f);
        printf("  [QDrawDetail 3] expanded schematic -> %s  (pdflatex %s)\n",
               QDRAW_SCHEMATIC_FILE, QDRAW_SCHEMATIC_FILE);
        return;
    }

    /* Levels 1,4,5 BUILD a representative circuit; the oracle/diffuser emitters
     * support N <= KNAPSACK_NMAX, so guard (callKnapsack may see larger N -> -3). */
    if (N > KNAPSACK_NMAX) {
        fprintf(stderr, "callKnapsack: QDrawDetail %d needs a built circuit but N=%d "
                "exceeds Nmax=%d; use level 2 or 3 for a schematic.\n",
                level, N, KNAPSACK_NMAX);
        return;
    }

    uint32_t Tdraw = 1;
    int j = 1;
    const char *qasm_path = QDRAW_QASM_FILE;
    if (level >= 5) {                 /* full depth: textbook single-target schedule */
        double jr = (M_PI / 4.0) * sqrt((double)((size_t)1 << N));
        j = (int)ceil(jr); if (j < 1) j = 1;
        qasm_path = QDRAW_QASM_FULL_FILE;
    }

    QkCircuit *qc = build_grover_circuit(&g, Tdraw, j);
    if (!qc) { fprintf(stderr, "callKnapsack: QDrawDetail %d: circuit not built\n", level); return; }

    if (level == 1) {                 /* TEXT: gate-level ASCII via the C API drawer */
        QkCircuitDrawerConfig cfg = { true, true, (size_t)160 };
        char *txt = qk_circuit_draw(qc, &cfg);
        FILE *f = fopen(QDRAW_TEXT_FILE, "w");
        if (f && txt) {
            fputs(txt, f);
            printf("  [QDrawDetail 1] gate-level text -> %s\n", QDRAW_TEXT_FILE);
        } else {
            fprintf(stderr, "callKnapsack: QDrawDetail 1 could not draw/open text\n");
        }
        if (f)   fclose(f);
        if (txt) qk_str_free(txt);
    } else {                          /* levels 4,5: gate-level OpenQASM-3 export */
        FILE *f = fopen(qasm_path, "w");
        if (f) {
            int rc = write_qasm(f, qc);
            fclose(f);
            if (rc == 0)
                printf("  [QDrawDetail %d] OpenQASM-3 (%d iteration%s) -> %s\n"
                       "      render B&W figure: python3 draw_circuit.py --qasm %s --pdf\n",
                       level, j, (j == 1 ? "" : "s"), qasm_path, qasm_path);
            else
                fprintf(stderr, "callKnapsack: QDrawDetail %d write_qasm failed\n", level);
        } else {
            perror("callKnapsack: QDrawDetail open qasm");
        }
    }

    qk_circuit_free(qc);
}


/*
 * callKnapsack — OpenModelica entry point: validate the inputs, run the selected solver path, return the standard result. Void; every output is a pointer.
 * Inputs : Npatents, Values[N], Costs[N], MaxCost; QDrawDetail: 0..5; SolverPath: 0 sim, 1 Aer, 2 QPU; NStates: must equal 2^Npatents. Values, Costs: non-negative integers stored as doubles (rounded to nearest; negatives clamp to 0); the register widths follow the sums, and no bound on their magnitude is enforced.
 * Outputs: PatentIndices[N]: 1 = item selected; ExStatus: 0 found, 1 verification run, -1 no feasible, -2 invalid, -3 too large, -6 timed out; PSubset[NStates]: probability of each subset; OracleCalls, Shots, Kind (0 exact final circuit, 1 verification shots)
 */
void callKnapsack(int           Npatents,
                  const double *Values,
                  const double *Costs,
                  double        MaxCost,
                  int          *PatentIndices,
                  int          *ExStatus,
                  int           QDrawDetail,
                  int           SolverPath,
                  int           NStates,        /* = 2^Npatents: size of PSubset (caller-allocated) */
                  double       *PSubset,        /* OUT: P(subset s), s = bitmask, bit i = item i      */
                  int          *OracleCalls,    /* OUT: path 0 oracle calls in the search; 0 otherwise */
                  int          *Shots,          /* OUT: paths 1/2 shots measured; 0 otherwise          */
                  int          *Kind)           /* OUT: 0 exact final-circuit distribution (path 0),
                                                 *      1 verification shots (paths 1/2)             */
{
    /* ── The standard result block starts empty, whatever happens below. ──────── */
    *OracleCalls = 0; *Shots = 0; *Kind = 0;
    if (NStates > 0 && PSubset) for (int s = 0; s < NStates; s++) PSubset[s] = 0.0;

    /* ── Input validation ──────────────────────────────────────────────── */
    if (Npatents <= 0 || Npatents > MAX_ITEMS) {
        fprintf(stderr,
                "callKnapsack: Npatents=%d out of range [1, %d]\n",
                Npatents, MAX_ITEMS);
        *ExStatus = -2;
        return;
    }

    /* ── Initialise PatentIndices to all-zero ──────────────────────────── */
    for (int i = 0; i < Npatents; i++)
        PatentIndices[i] = 0;

    /* ── Optional circuit drawing (QDrawDetail levels 0–5; see write_circuit_drawing) ─────── */
    write_circuit_drawing(QDrawDetail, Npatents, Values, Costs, MaxCost);

    /* ── Allocate probability scratch buffer ───────────────────────────── */
    size_t  num_states    = (size_t)1 << Npatents;
    if (NStates != (int)num_states || !PSubset) {
        fprintf(stderr, "callKnapsack: NStates=%d must equal 2^Npatents=%zu\n", NStates, num_states);
        *ExStatus = -2;
        return;
    }
    double *SolutionProbs = malloc(num_states * sizeof(double));
    if (!SolutionProbs) {
        fputs("callKnapsack: malloc failed for SolutionProbs\n", stderr);
        *ExStatus = -2;
        return;
    }

    /* ── Solve the knapsack ─────────────────────────────────────────────── */
    int tmp_indices[MAX_ITEMS];
    int out_count = 0, verification = 0;

    kp_result res = {0};
    double best = solve_knapsack(Npatents, Costs, Values, MaxCost,
                                 tmp_indices, &out_count, SolutionProbs, SolverPath,
                                 &verification, &res);
    if (best >= 0.0)                                     /* standard block only when a subset came back */
        for (size_t s = 0; s < num_states; s++) PSubset[s] = SolutionProbs[s];
    free(SolutionProbs);

    if (best == -6.0) {             /* paths 1/2: no results file within the timeout */
        *ExStatus = -6;
        return;
    }
    if (best == -2.0) {             /* bad path, I/O, malformed or mismatched results */
        *ExStatus = -2;
        return;
    }

    if (best <= -2.5) {       /* solve_knapsack sentinel -3.0: too large for the solver */
        *ExStatus = -3;       /* problem exceeds the quantum solver's Nmax */
        return;
    }
    if (best < 0.0) {
        *ExStatus = -1;   /* no feasible solution */
        return;
    }

    /* ── Convert index list → binary indicator ──────────────────────────── */
    for (int k = 0; k < out_count; k++)
        PatentIndices[tmp_indices[k]] = 1;

    *ExStatus    = verification ? 1 : 0;   /* 1 = answer from a phase-1 VERIFICATION run */
    *Kind        = res.verification;
    *OracleCalls = (int)res.oracle_calls;
    *Shots       = (int)res.shots;
}
