/*
 * oracle_test.c — white-box check of the oracle (emit_oracle) against an independent classical predicate. Includes knapsack.c directly.
 *
 * Modules:
 *   check                  record one pass/fail
 *   predicate              classical predicate: fits the budget and reaches the threshold
 *   run_oracle_check       oracle on the uniform superposition versus the predicate
 *   main                   the test cases

 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-10
 */

#include "knapsack.c"   /* white-box: exposes static emit_oracle + simulate_circuit */
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

#define MAXW 8          /* generous upper bound on N / register widths used here */


/*
 * predicate — Classical predicate, independent of the circuit: fits the budget and reaches the threshold.
 * Inputs : x, N, costs, values, W, T
 * Outputs: 1 or 0
 */
static int predicate(uint32_t x, int N, const uint32_t *costs, const uint32_t *values,
                     uint32_t W, uint32_t T)
{
    uint32_t c = 0, v = 0;
    for (int i = 0; i < N; i++)
        if ((x >> i) & 1u) { c += costs[i]; v += values[i]; }
    return (c <= W) && (v >= T);
}


/*
 * run_oracle_check — Build the layout, apply the oracle to the uniform item superposition, simulate, compare with the predicate.
 * Inputs : label, N, costs, values, m_cost, m_val, W, T
 * Outputs: checks recorded
 */
static void run_oracle_check(const char *label, int N,
        const uint32_t *costs, const uint32_t *values,
        uint32_t m_cost, uint32_t m_val, uint32_t W, uint32_t T)
{
    uint32_t m_carry = (m_cost > m_val) ? m_cost : m_val;

    /* assign qubit indices */
    uint32_t item[MAXW], cost_q[MAXW], value_q[MAXW], carry_q[MAXW], feas, valok;
    uint32_t b = 0;
    for (int i = 0; i < N; i++)             item[i]    = b++;
    for (uint32_t i = 0; i < m_cost; i++)   cost_q[i]  = b++;
    for (uint32_t i = 0; i < m_val; i++)    value_q[i] = b++;
    for (uint32_t i = 0; i < m_carry; i++)  carry_q[i] = b++;
    feas  = b++;
    valok = b++;
    int NQ = (int)b;

    QkCircuit *qc = qk_circuit_new(NQ, 0);
    /* uniform superposition over the item register */
    for (int i = 0; i < N; i++) { uint32_t q[1] = { item[i] }; qk_circuit_gate(qc, QkGate_H, q, NULL); }
    /* the oracle under test */
    emit_oracle(qc, item, N, costs, values,
                cost_q, m_cost, value_q, m_val, carry_q, feas, valok, W, T);

    double complex *sv = sv_alloc(NQ);
    int ng = simulate_circuit(qc, sv, NQ);

    double amp = 1.0 / sqrt((double)((size_t)1 << N));
    size_t dim = (size_t)1 << NQ;
    size_t item_states = (size_t)1 << N;

    int leak = 0, sign_err = 0, mag_err = 0, populated = 0, marked = 0;
    for (size_t s = 0; s < dim; s++) {
        double re = creal(sv[s]), im = cimag(sv[s]);
        double pr = re * re + im * im;
        if (pr < 1e-12) continue;                 /* unpopulated */
        populated++;
        if (s >= item_states) { leak++; continue; }   /* ancilla != 0 → uncompute failed */
        uint32_t x = (uint32_t)s;                 /* ancillas are 0 here */
        int m = predicate(x, N, costs, values, W, T);
        if (m) marked++;
        double expect = m ? -amp : amp;
        if (fabs(sqrt(pr) - amp) > 1e-6)                  mag_err++;
        if (fabs(re - expect) > 1e-6 || fabs(im) > 1e-6)  sign_err++;
    }

    /* independently-derived classical marked count, for the report and the check */
    int classical_marked = 0;
    for (uint32_t x = 0; x < (uint32_t)item_states; x++)
        if (predicate(x, N, costs, values, W, T)) classical_marked++;

    int ok = (ng >= 0) && (leak == 0) && (sign_err == 0) && (mag_err == 0)
             && (populated == (int)item_states) && (marked == classical_marked);

    if (!ok)
        printf("    (debug NQ=%d populated=%d (want %zu) leak=%d sign_err=%d mag_err=%d "
               "marked=%d classical=%d)\n",
               NQ, populated, item_states, leak, sign_err, mag_err, marked, classical_marked);

    char full[160];
    snprintf(full, sizeof full,
             "%-26s [N=%d W=%2u T=%2u -> marks %d/%zu, ancillas clean]",
             label, N, W, T, classical_marked, item_states);
    check(full, ok);

    free(sv);
    qk_circuit_free(qc);
}

/*
 * main — Run the test cases.
 * Outputs: exit code: 0 all passed
 */
int main(void)
{
    printf("Grover oracle self-check\n");
    printf("================================\n");
    printf("  oracle phase-marks EXACTLY the classically feasible/high-value states,\n");
    printf("  AND returns every ancilla to |0> (rules IR1 and IR2 of the engineering document,\n"
           "  Knapsack_Documentation.pdf, Correctness and honesty rules).\n");
    printf("  [the N=4 cases simulate 20 qubits each; allow a few seconds per case]\n\n");

    /* ── A. Tiny hand-checked instance (N=2): c[2,3] v[3,4], W=3 T=4 → marks {item1}. ── */
    printf("  A) tiny hand-checked instance:\n");
    {
        const uint32_t c[2] = { 2, 3 }, v[2] = { 3, 4 };
        run_oracle_check("c[2,3] v[3,4]", 2, c, v, /*m_cost=*/3, /*m_val=*/3, /*W=*/3, /*T=*/4);
    }

    /* ── B. N=4 paper instance: c[2,3,4,5] (Σ14 → m_cost 4), v[3,4,5,7] (Σ19 → m_val 5),
     *       swept across representative (W,T): all-feasible, two mid thresholds, and an
     *       empty mark (threshold above the total value). ───────────────────────────── */
    printf("\n  B) N=4 paper instance c[2,3,4,5] v[3,4,5,7] across several (W,T):\n");
    {
        const uint32_t c[4] = { 2, 3, 4, 5 }, v[4] = { 3, 4, 5, 7 };
        run_oracle_check("all feasible, value>=1", 4, c, v, 4, 5, 14,  1);
        run_oracle_check("budget 9, value>=12",    4, c, v, 4, 5,  9, 12);
        run_oracle_check("budget 5, value>=7",     4, c, v, 4, 5,  5,  7);
        run_oracle_check("threshold>total (none)", 4, c, v, 4, 5,  7, 20);
    }

    printf("================================\n");
    printf("%s\n", g_fail ? "SOME ORACLE TESTS FAILED"
                          : "ALL ORACLE TESTS PASSED");
    return g_fail;
}
