/*
 * solver_test.c — white-box test of the quantum maximum-finding solver (grover_knapsack) against the classical optimum. Includes knapsack.c directly.
 *
 * Modules:
 *   check                  record one pass/fail
 *   classical_opt          independent classical optimum
 *   run_solver_check       solver versus the classical optimum over several seeds
 *   main                   the test cases

 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-10
 */

#include "knapsack.c"   /* white-box: exposes static grover_knapsack + the sim */
#include <math.h>
#include <time.h>

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
 * classical_opt — Independent classical optimum over the feasible subsets.
 * Inputs : N, costs, values, W
 * Outputs: *out_mask, *out_value
 */
static void classical_opt(int N, const uint32_t *costs, const uint32_t *values, uint32_t W,
                          uint32_t *out_mask, uint32_t *out_value)
{
    uint32_t best_mask = 0, best_val = 0;
    for (uint32_t x = 0; x < (1u << N); x++) {
        uint32_t c = 0, v = 0;
        for (int i = 0; i < N; i++) if ((x >> i) & 1u) { c += costs[i]; v += values[i]; }
        if (c <= W && v > best_val) { best_val = v; best_mask = x; }
    }
    *out_mask = best_mask; *out_value = best_val;
}

/*
 * run_solver_check — Run the quantum solver over several seeds and compare with the classical optimum.
 * Inputs : label, N, costs, values, W, K, n_seeds
 * Outputs: checks recorded
 */
static void run_solver_check(const char *label, int N,
        const uint32_t *costs, const uint32_t *values, uint32_t W, int K, int n_seeds)
{
    uint32_t opt_mask, opt_val;
    classical_opt(N, costs, values, W, &opt_mask, &opt_val);

    clock_t t0 = clock();
    int hits = 0; long sum_oracle = 0;
    gstats st;
    for (int s = 0; s < n_seeds; s++) {
        uint32_t mask, val;
        grover_knapsack(N, costs, values, W,
                        0x1234ULL + (uint64_t)s * 0x9E3779B97F4A7C15ULL, K, &st, &mask, &val);
        sum_oracle += st.total_oracle_calls;
        /* the solver must return the OPTIMAL VALUE via a feasible subset achieving it */
        uint32_t c = 0, v = 0;
        for (int i = 0; i < N; i++) if ((mask >> i) & 1u) { c += costs[i]; v += values[i]; }
        if (val == opt_val && c <= W && v == val) hits++;
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;

    int ok = (hits == n_seeds);
    if (!ok)
        printf("    (debug: optimum value=%u mask=0x%X; solver matched %d of %d seeds)\n",
               opt_val, opt_mask, hits, n_seeds);

    long avg_oracle = sum_oracle / (n_seeds > 0 ? n_seeds : 1);
    char full[200];
    snprintf(full, sizeof full,
             "%-24s [opt %u (0x%X); K=%d hit %d/%d, ~%ld oracle/solve, %.1fs]",
             label, opt_val, opt_mask, K, hits, n_seeds, avg_oracle, secs);
    check(full, ok);
}

/*
 * main — Run the test cases.
 * Outputs: exit code: 0 all passed
 */
int main(void)
{
    printf("Quantum maximum-finding solver tests (BBHT + Durr-Hoyer)\n");
    printf("==========================================================\n");
    printf("  the QUANTUM solver builds+simulates the real oracle+diffuser circuit,\n");
    printf("  measures, and runs BBHT / Durr-Hoyer; compared to the classical optimum.\n\n");

    /* A. small instance (N=3), many seeds — statistical confidence (fast). */
    printf("  A) N=3 instance, many seeds:\n");
    {
        const uint32_t c[3] = { 1, 1, 3 }, v[3] = { 2, 2, 3 };
        run_solver_check("c[1,1,3] v[2,2,3] W=3", 3, c, v, 3, GROVER_K, 20);
    }

    /* B. N=4 paper instance, a few seeds — the real instance (slower, 20 qubits). */
    printf("\n  B) N=4 paper instance, few seeds:\n");
    {
        const uint32_t c[4] = { 2, 3, 4, 5 }, v[4] = { 3, 4, 5, 7 };
        run_solver_check("c[2,3,4,5] v[3,4,5,7] W=10", 4, c, v, 10, GROVER_K, 2);
    }

    /* C. N=5 — the smallest N whose diffuser uses the ancilla ladder (F1, 2026-10-10). */
    printf("\n  C) N=5 instance (TC1 N=5), few seeds — exercises the N>=5 diffuser:\n");
    {
        const uint32_t c[5] = { 2, 3, 1, 3, 2 }, v[5] = { 2, 1, 3, 1, 3 };
        run_solver_check("c[2,3,1,3,2] v[2,1,3,1,3] W=5", 5, c, v, 5, GROVER_K, 2);
    }

    printf("==========================================================\n");
    printf("%s\n", g_fail ? "SOME SOLVER TESTS FAILED"
                          : "ALL SOLVER TESTS PASSED");
    return g_fail;
}
