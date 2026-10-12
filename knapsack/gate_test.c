/*
 * gate_test.c — white-box tests of the statevector gate handlers (sv_mcx, sv_mcz) and the simulate_circuit() dispatch. Includes knapsack.c directly.
 *
 * Modules:
 *   check                  record one pass/fail
 *   amp_is                 compare an amplitude with a tolerance
 *   main                   the test cases

 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-10
 */

#include "knapsack.c"   /* white-box: exposes static sv_* and simulate_circuit */

/* ── tiny test harness ───────────────────────────────────────────────────── */
static int g_fail = 0;

/*
 * check — Record one pass/fail result.
 * Inputs : what: label; ok: condition
 * Outputs: counters updated; line printed
 */
static void check(const char *what, int ok)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail = 1;
}

/*
 * amp_is — Compare an amplitude with (re + i im) to a tolerance.
 * Inputs : z, re, im
 * Outputs: 1 or 0
 */
static int amp_is(double complex z, double re, double im)
{
    return fabs(creal(z) - re) < 1e-12 && fabs(cimag(z) - im) < 1e-12;
}

/*
 * main — Run the test cases.
 * Outputs: exit code: 0 all passed
 */
int main(void)
{
    printf("Statevector gate-handler unit tests\n");
    printf("================================\n");

    /* ── sv_mcx as Toffoli (2 controls): |q2 q1 q0> = |110> → |111> ──────────
     * Prepare |110> by flipping q0 and q1 (index 0b011 = 3); apply CCX with
     * controls {q0,q1}, target q2; expect amplitude to move to index 0b111 = 7. */
    {
        int n = 3;
        double complex *sv = sv_alloc(n);     /* |000>, sv[0] = 1 */
        sv_x(sv, n, 0);
        sv_x(sv, n, 1);                        /* now |110> (index 3) */
        uint32_t ctrl[2] = {0, 1};
        sv_mcx(sv, n, ctrl, 2, 2);
        check("CCX flips target when both controls set (|110>->|111>)",
              amp_is(sv[7], 1, 0) && amp_is(sv[3], 0, 0));
        free(sv);
    }

    /* ── sv_mcx is identity when a control is 0 ──────────────────────────────
     * Prepare |010> (only q1 set, index 2); CCX{q0,q1}→q2 must do nothing. */
    {
        int n = 3;
        double complex *sv = sv_alloc(n);
        sv_x(sv, n, 1);
        uint32_t ctrl[2] = {0, 1};
        sv_mcx(sv, n, ctrl, 2, 2);
        check("CCX is identity when a control is 0 (|010> unchanged)",
              amp_is(sv[2], 1, 0));
        free(sv);
    }

    /* ── sv_mcx with 3 controls (C3X): |1110> → |1111> ───────────────────────
     * Prepare q0,q1,q2 set (index 7); C3X controls {q0,q1,q2}, target q3;
     * expect index 0b1111 = 15. */
    {
        int n = 4;
        double complex *sv = sv_alloc(n);
        sv_x(sv, n, 0); sv_x(sv, n, 1); sv_x(sv, n, 2);
        uint32_t ctrl[3] = {0, 1, 2};
        sv_mcx(sv, n, ctrl, 3, 3);
        check("C3X flips target when all 3 controls set (|1110>->|1111>)",
              amp_is(sv[15], 1, 0) && amp_is(sv[7], 0, 0));
        free(sv);
    }

    /* ── sv_mcz phase flip: CCZ on |111> applies -1 ─────────────────────────── */
    {
        int n = 3;
        double complex *sv = sv_alloc(n);
        sv_x(sv, n, 0); sv_x(sv, n, 1); sv_x(sv, n, 2);   /* |111> (index 7) */
        uint32_t qs[3] = {0, 1, 2};
        sv_mcz(sv, n, qs, 3);
        check("CCZ applies -1 phase on |111>", amp_is(sv[7], -1, 0));
        free(sv);
    }

    /* ── sv_mcz is identity unless ALL listed qubits are 1 ──────────────────── */
    {
        int n = 3;
        double complex *sv = sv_alloc(n);
        sv_x(sv, n, 0); sv_x(sv, n, 1);                   /* |110> (index 3) */
        uint32_t qs[3] = {0, 1, 2};
        sv_mcz(sv, n, qs, 3);
        check("CCZ is identity unless all qubits are 1 (|110> unchanged)",
              amp_is(sv[3], 1, 0));
        free(sv);
    }

    {
        int n = 4;
        QkCircuit *qc = qk_circuit_new((uint32_t)n, 0);
        uint32_t a[1] = {0}, b[1] = {1}, c[1] = {2}, q4[4] = {0, 1, 2, 3};
        qk_circuit_gate(qc, QkGate_X,   a,  NULL);
        qk_circuit_gate(qc, QkGate_X,   b,  NULL);
        qk_circuit_gate(qc, QkGate_X,   c,  NULL);
        qk_circuit_gate(qc, QkGate_C3X, q4, NULL);
        double complex *sv = sv_alloc(n);
        int ng = simulate_circuit(qc, sv, n);
        check("simulate_circuit applies X and C3X(name=\"mcx\") with no silent skip",
              ng == 4 && amp_is(sv[15], 1, 0));
        free(sv);
        qk_circuit_free(qc);
    }

    printf("================================\n");
    printf("%s\n", g_fail ? "SOME GATE TESTS FAILED" : "ALL GATE TESTS PASSED");
    return g_fail;
}
