/*
 * diffuser_test.c — white-box tests of the Grover diffuser (emit_diffuser). Includes knapsack.c directly.
 *
 * Modules:
 *   check                  record one pass/fail
 *   run_reflection_check   the diffuser's reflection identities
 *   predicate              classical marking predicate
 *   run_iteration_check    one Grover iteration amplifies the marked subsets
 *   main                   the test cases

 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-10
 */

#include "knapsack.c"   /* white-box: exposes static emit_diffuser / emit_oracle */
#include <math.h>

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

#define MAXW 8

/* ── A. reflection identities (item register only — fast) ─────────────────────── */

/*
 * run_reflection_check — Check the diffuser's reflection identities on the item register.
 * Inputs : N
 * Outputs: checks recorded
 */
static void run_reflection_check(int N)
{
    uint32_t item[MAXW], anc[MAXW];
    for (int i = 0; i < N; i++) item[i] = (uint32_t)i;
    int n_anc = (N >= 5) ? N - 3 : 0;                 /* the ladder's clean ancillas */
    for (int i = 0; i < n_anc; i++) anc[i] = (uint32_t)(N + i);
    int NQ = N + n_anc;
    size_t NN = (size_t)1 << N, dim = (size_t)1 << NQ;

    /* A1: D|s> = -|s>  →  every item amplitude = -1/sqrt(2^N), ancillas back to |0> */
    {
        QkCircuit *qc = qk_circuit_new((uint32_t)NQ, 0);
        for (int i = 0; i < N; i++) { uint32_t q[1] = { item[i] }; qk_circuit_gate(qc, QkGate_H, q, NULL); }
        int rc = emit_diffuser(qc, item, N, anc, n_anc);
        double complex *sv = sv_alloc(NQ);
        int ng = simulate_circuit(qc, sv, NQ);
        double want = -1.0 / sqrt((double)NN);
        int ok = (rc == 0) && (ng >= 0);
        for (size_t s = 0; s < dim; s++) {
            double w = (s < NN) ? want : 0.0;          /* ancilla != 0 must carry nothing */
            if (fabs(creal(sv[s]) - w) > 1e-9 || fabs(cimag(sv[s])) > 1e-9) ok = 0;
        }
        char l[96]; snprintf(l, sizeof l, "N=%d  D|s> = -|s>   (uniform is the fixed axis; %d ancilla(s) clean)", N, n_anc);
        check(l, ok);
        free(sv); qk_circuit_free(qc);
    }
    /* A2: D|0> = inversion about the mean  →  amp_0 = 1-2/2^N, others -2/2^N */
    {
        QkCircuit *qc = qk_circuit_new((uint32_t)NQ, 0);
        int rc = emit_diffuser(qc, item, N, anc, n_anc);
        double complex *sv = sv_alloc(NQ);
        int ng = simulate_circuit(qc, sv, NQ);
        double a0 = 1.0 - 2.0 / (double)NN, aj = -2.0 / (double)NN;
        int ok = (rc == 0) && (ng >= 0);
        for (size_t s = 0; s < dim; s++) {
            double w = (s >= NN) ? 0.0 : (s == 0) ? a0 : aj;
            if (fabs(creal(sv[s]) - w) > 1e-9 || fabs(cimag(sv[s])) > 1e-9) ok = 0;
        }
        char l[96]; snprintf(l, sizeof l, "N=%d  D|0> = inversion about the mean", N);
        check(l, ok);
        free(sv); qk_circuit_free(qc);
    }
}

/* ── B. one Grover iteration amplifies the marked subspace ─────────────────────── */

/*
 * predicate — Classical marking predicate: fits the budget and reaches the threshold.
 * Inputs : x, N, c, v, W, T
 * Outputs: 1 or 0
 */
static int predicate(uint32_t x, int N, const uint32_t *c, const uint32_t *v,
                     uint32_t W, uint32_t T)
{
    uint32_t cc = 0, vv = 0;
    for (int i = 0; i < N; i++) if ((x >> i) & 1u) { cc += c[i]; vv += v[i]; }
    return (cc <= W) && (vv >= T);
}

/*
 * run_iteration_check — Check that one Grover iteration raises the probability of the marked subsets.
 * Inputs : label, N, costs, values, m_cost, m_val, W, T
 * Outputs: checks recorded
 */
static void run_iteration_check(const char *label, int N,
        const uint32_t *costs, const uint32_t *values,
        uint32_t m_cost, uint32_t m_val, uint32_t W, uint32_t T)
{
    uint32_t m_carry = (m_cost > m_val) ? m_cost : m_val;
    uint32_t item[MAXW], cost_q[MAXW], value_q[MAXW], carry_q[MAXW], feas, valok;
    uint32_t b = 0;
    for (int i = 0; i < N; i++)             item[i]    = b++;
    for (uint32_t i = 0; i < m_cost; i++)   cost_q[i]  = b++;
    for (uint32_t i = 0; i < m_val; i++)    value_q[i] = b++;
    for (uint32_t i = 0; i < m_carry; i++)  carry_q[i] = b++;
    feas = b++; valok = b++;
    int NQ = (int)b;
    size_t item_states = (size_t)1 << N;
    uint32_t anc[3 * MAXW]; int n_anc = 0;             /* the oracle's registers, |0> after it */
    for (uint32_t i = 0; i < m_cost;  i++) anc[n_anc++] = cost_q[i];
    for (uint32_t i = 0; i < m_val;   i++) anc[n_anc++] = value_q[i];
    for (uint32_t i = 0; i < m_carry; i++) anc[n_anc++] = carry_q[i];

    QkCircuit *qc = qk_circuit_new(NQ, 0);
    for (int i = 0; i < N; i++) { uint32_t q[1] = { item[i] }; qk_circuit_gate(qc, QkGate_H, q, NULL); }
    emit_oracle(qc, item, N, costs, values, cost_q, m_cost, value_q, m_val, carry_q, feas, valok, W, T);
    int rcd = emit_diffuser(qc, item, N, anc, n_anc);

    double complex *sv = sv_alloc(NQ);
    int ng = simulate_circuit(qc, sv, NQ);

    /* independently-counted number of marked states M */
    int M = 0;
    for (uint32_t x = 0; x < (uint32_t)item_states; x++)
        if (predicate(x, N, costs, values, W, T)) M++;

    /* simulated total marked probability + ancilla-clean check */
    double marked_pr = 0.0, total_pr = 0.0;
    int leak = 0;
    size_t dim = (size_t)1 << NQ;
    for (size_t s = 0; s < dim; s++) {
        double pr = creal(sv[s]) * creal(sv[s]) + cimag(sv[s]) * cimag(sv[s]);
        if (pr < 1e-15) continue;
        total_pr += pr;
        if (s >= item_states) { leak++; continue; }
        if (predicate((uint32_t)s, N, costs, values, W, T)) marked_pr += pr;
    }

    /* analytic Grover value after one iteration: sin^2(3θ), sinθ = sqrt(M/2^N) */
    double theta  = asin(sqrt((double)M / (double)item_states));
    double expect = sin(3.0 * theta); expect *= expect;

    int ok = (rcd == 0) && (ng >= 0) && (leak == 0)
             && (fabs(total_pr  - 1.0)    < 1e-6)
             && (fabs(marked_pr - expect) < 1e-6);

    if (!ok)
        printf("    (debug NQ=%d M=%d leak=%d total=%.6f marked=%.6f expect=%.6f)\n",
               NQ, M, leak, total_pr, marked_pr, expect);

    char full[160];
    snprintf(full, sizeof full,
             "%-20s [M=%d/%zu -> P=%.4f after 1 iter, want sin^2(3theta)=%.4f]",
             label, M, item_states, marked_pr, expect);
    check(full, ok);

    free(sv); qk_circuit_free(qc);
}

/*
 * main — Run the test cases.
 * Outputs: exit code: 0 all passed
 */
int main(void)
{
    printf("Grover diffuser tests\n");
    printf("=============================\n\n");

    printf("  A) reflection identities (diffuser == I - 2|s><s|) for every N <= KNAPSACK_NMAX = %d:\n", KNAPSACK_NMAX);
    for (int N = 2; N <= KNAPSACK_NMAX; N++)   /* emit_cnz: CZ, CCZ, H.C3X.H, then the ancilla ladder */
        run_reflection_check(N);

    printf("\n  B) one Grover iteration (oracle then diffuser) amplifies to sin^2(3theta):\n");
    {
        const uint32_t c[4] = { 2, 3, 4, 5 }, v[4] = { 3, 4, 5, 7 };
        run_iteration_check("M=1 (full set)", 4, c, v, 4, 5, 14, 19);  /* only {ABCD}, value 19 */
        run_iteration_check("M=2",            4, c, v, 4, 5, 14, 16);  /* {BCD}=16, {ABCD}=19   */
    }
    {   /* N=5 and N=7: the ancilla-ladder C^(N-1)Z inside a real iteration (F1, 2026-10-10) */
        const uint32_t c5[5] = { 2, 3, 1, 3, 2 }, v5[5] = { 2, 1, 3, 1, 3 };   /* TC1 N=5, W=5 */
        run_iteration_check("N=5 TC1 T=7",    5, c5, v5, 4, 4, 5, 7);
        const uint32_t c7[7] = { 2, 1, 1, 3, 3, 2, 1 }, v7[7] = { 1, 1, 3, 1, 2, 3, 2 }; /* TC1 N=7, W=6 */
        run_iteration_check("N=7 TC1 T=9",    7, c7, v7, 4, 4, 6, 9);
    }

    printf("=============================\n");
    printf("%s\n", g_fail ? "SOME DIFFUSER TESTS FAILED"
                          : "ALL DIFFUSER TESTS PASSED");
    return g_fail;
}
