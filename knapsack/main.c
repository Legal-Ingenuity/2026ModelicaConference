/*
 * main.c — black-box test of callKnapsack(), the OpenModelica entry point, without OpenModelica.
 *
 * Modules:
 *   run_test               call callKnapsack() on one instance and check status, cost and value
 *   main                   the test cases

 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-10
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Forward declaration — must stay in sync with knapsack.c and ModKnapsack.mo */
void callKnapsack(int           Npatents,
                  const double *Values,
                  const double *Costs,
                  double        MaxCost,
                  int          *PatentIndices,
                  int          *ExStatus,
                  int           QDrawDetail,
                  int           SolverPath,    /* 0 = pure-C simulation (D5) */
                  int           NStates,       /* = 2^Npatents (D6) */
                  double       *PSubset,       /* OUT: P(subset), size NStates */
                  int          *OracleCalls,   /* OUT */
                  int          *Shots,         /* OUT */
                  int          *Kind);         /* OUT: 0 exact final-circuit distribution, 1 verification shots */


/*
 * run_test — Call callKnapsack() on one instance; print and check ExStatus, cost and value.
 * Inputs : label; Npatents, Values, Costs, MaxCost; expect_status, expect_cost, expect_value
 * Outputs: pass/fail printed; returns 1 on pass, 0 on failure (the caller counts)
 */
static int run_test(
    const char   *label,
    int           Npatents,
    const double *Values,
    const double *Costs,
    double        MaxCost,
    int           expect_status,
    double        expect_cost,
    double        expect_value)
{
    printf("══════════════════════════════════════════════════════════════\n");
    printf("  TEST: %s\n", label);
    printf("  Npatents=%d  MaxCost=%.2f\n\n", Npatents, MaxCost);

    int *PatentIndices = calloc((size_t)(Npatents > 0 ? Npatents : 1), sizeof(int));
    int  ExStatus      = 99;

    int     NStates = (Npatents > 0 && Npatents < 31) ? (1 << Npatents) : 0;
    double *PSubset = calloc((size_t)(NStates > 0 ? NStates : 1), sizeof(double));
    int     OracleCalls = 0, Shots = 0, Kind = 0;
    callKnapsack(Npatents, Values, Costs, MaxCost,
                 PatentIndices, &ExStatus, /*QDrawDetail=*/0, /*SolverPath=*/0,
                 NStates, PSubset, &OracleCalls, &Shots, &Kind);
    int psubset_ok = 1;
    if (ExStatus == 0) {                                   /* the distribution must sum to 1 */
        double sum = 0.0; for (int s = 0; s < NStates; s++) sum += PSubset[s];
        printf("  PSubset sums to %.6f   OracleCalls=%d Shots=%d Kind=%d\n", sum, OracleCalls, Shots, Kind);
        if (fabs(sum - 1.0) > 1e-9) { printf("  [FAIL] PSubset does not sum to 1\n"); psubset_ok = 0; }
    }
    free(PSubset);

    /* Mirror Modelica's post-processing: compute cost and value from indices */
    double ActualCost  = 0.0;
    double ActualValue = 0.0;
    for (int i = 0; i < Npatents; i++) {
        if (PatentIndices[i] == 1) {
            ActualCost  += Costs[i];
            ActualValue += Values[i];
        }
    }

    /* Print outcome */
    printf("  ExStatus = %d  ", ExStatus);
    switch (ExStatus) {
        case  0: printf("(solution found)\n");             break;
        case -1: printf("(no feasible solution)\n");       break;
        case -2: printf("(invalid input / alloc error)\n");break;
        case -3: printf("(problem too large for quantum solver)\n"); break;
        default: printf("(unexpected status)\n");          break;
    }

    if (ExStatus == 0) {
        printf("  ActualCost  = %.2f  (MaxCost limit = %.2f)\n",
               ActualCost, MaxCost);
        printf("  ActualValue = %.2f\n", ActualValue);

        printf("  PatentIndices = [");
        for (int i = 0; i < Npatents; i++) {
            if (i) printf(", ");
            printf("%d", PatentIndices[i]);
        }
        printf("]\n");

        printf("  Selected items (0-based idx : cost / value):");
        int any = 0;
        for (int i = 0; i < Npatents; i++) {
            if (PatentIndices[i] == 1) {
                printf("  item %d : %.2f / %.2f", i, Costs[i], Values[i]);
                any = 1;
            }
        }
        if (!any) printf("  (none)");
        printf("\n");
    }

    /* Verify */
    int pass = psubset_ok;
    if (ExStatus != expect_status) {
        printf("  [FAIL] expected ExStatus=%d, got %d\n", expect_status, ExStatus);
        pass = 0;
    }
    if (expect_status == 0) {
        if (fabs(ActualCost - expect_cost) > 1e-9) {
            printf("  [FAIL] expected ActualCost=%.2f, got %.2f\n",
                   expect_cost, ActualCost);
            pass = 0;
        }
        if (fabs(ActualValue - expect_value) > 1e-9) {
            printf("  [FAIL] expected ActualValue=%.2f, got %.2f\n",
                   expect_value, ActualValue);
            pass = 0;
        }
    }
    printf("  %s\n\n", pass ? "PASS" : "[FAIL]");

    free(PatentIndices);
    return pass;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test cases
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * main — Run the test cases.
 * Outputs: a summary line; process exit code 0 if every case passed, 1 otherwise
 */
int main(void)
{
    int passed = 0, total = 0;
#define RUN(...) do { total++; passed += run_test(__VA_ARGS__); } while (0)
    /* ── TC1: Modelica baseline ───────────────────────────────────────────────
     * Identical parameters to KnapsackRun in ModKnapsack.mo.
     * Optimal: items 0+2 (costs 2+4=6, values 3+5=8).               */
    {
        double Values[] = {3, 4, 5, 7};
        double Costs[]  = {2, 3, 4, 5};
        RUN("TC1 — Modelica baseline (N=4, MaxCost=6)",
                 4, Values, Costs, 6.0,
                 /*expect*/ 0, 6.0, 8.0);
    }

    /* ── TC2: Take all items ─────────────────────────────────────────────────
     * MaxCost far exceeds total cost; all four items should be selected.
     * Total cost=14, total value=19.                                  */
    {
        double Values[] = {3, 4, 5, 7};
        double Costs[]  = {2, 3, 4, 5};
        RUN("TC2 — Take all (N=4, MaxCost=100)",
                 4, Values, Costs, 100.0,
                 /*expect*/ 0, 14.0, 19.0);
    }

    /* ── TC3: No feasible solution ───────────────────────────────────────────
     * MaxCost=1 is below the cheapest item cost (2); solver must return -1. */
    {
        double Values[] = {3, 4, 5, 7};
        double Costs[]  = {2, 3, 4, 5};
        RUN("TC3 — No feasible solution (N=4, MaxCost=1)",
                 4, Values, Costs, 1.0,
                 /*expect*/ -1, 0.0, 0.0);
    }

    /* ── TC4: Single item — fits ─────────────────────────────────────────────
     * N=1 is the smallest valid input.  Item fits; should be selected.  */
    {
        double Values[] = {9.5};
        double Costs[]  = {3.0};
        RUN("TC4 — Single item fits (N=1, MaxCost=5)",
                 1, Values, Costs, 5.0,
                 /*expect*/ 0, 3.0, 9.5);
    }

    /* ── TC5: Single item — does not fit ─────────────────────────────────────
     * N=1, item cost exceeds MaxCost; solver must return -1.           */
    {
        double Values[] = {9.5};
        double Costs[]  = {6.0};
        RUN("TC5 — Single item does not fit (N=1, MaxCost=5)",
                 1, Values, Costs, 5.0,
                 /*expect*/ -1, 0.0, 0.0);
    }

    /* ── TC6: Too large for the quantum solver ───────────────────────────────
     * N=8 exceeds Nmax=7; callKnapsack must return ExStatus=-3.           */
    {
        double Values[] = {10,  6,  5,  4,  3,  2,  1,  1};
        double Costs[]  = { 5,  4,  3,  2,  1,  1,  1,  1};
        RUN("TC6 — Larger problem, N=8, MaxCost=10",
                 8, Values, Costs, 10.0,
                 /*expect*/ -3, 0.0, 0.0);   /* N=8 > Nmax=7: quantum solver too large */
    }

    /* ── TC7: Invalid input (N=0) ────────────────────────────────────────────
     * N=0 is outside [1, MAX_ITEMS]; callKnapsack must return ExStatus=-2. */
    {
        RUN("TC7 — Invalid input (N=0)",
                 0, NULL, NULL, 10.0,
                 /*expect*/ -2, 0.0, 0.0);
    }

    printf("══════════════════════════════════════════════════════════════\n");
    if (passed == total) printf("  knapsack_test: %d of %d cases passed\n", passed, total);
    else                 printf("  knapsack_test: [FAIL] %d of %d cases failed\n", total - passed, total);
    return (passed == total) ? 0 : 1;
}
