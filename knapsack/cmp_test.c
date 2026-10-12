/*
 * cmp_test.c — white-box tests of the comparators (emit_flag_leq, emit_flag_geq, emit_compare_const). Includes knapsack.c directly.
 *
 * Modules:
 *   check                  record one pass/fail
 *   unique_state           find the single populated basis state
 *   run_cmp                one comparison in isolation
 *   sweep                  all register values against one bound
 *   main                   the test cases

 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-10
 */

#include "knapsack.c"   /* white-box: exposes static emit_flag_* and simulate_circuit */

/* ── tiny test harness (same style as gate_test.c / adder_test.c) ────────────── */
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

#define M   5                              /* comparison width (bits)              */
#define NQ  11                             /* 5 reg + 5 carry + 1 flag             */

static const uint32_t REG_Q[M]   = { 0, 1, 2, 3, 4 };   /* LSB = REG_Q[0]          */
static const uint32_t CARRY_Q[M] = { 5, 6, 7, 8, 9 };   /* carry out of bit j      */
static const uint32_t FLAG_Q     = 10;                  /* feasibility flag        */


/*
 * unique_state — Find the single populated basis state of a statevector.
 * Inputs : sv, n
 * Outputs: its index; *p_count = number of populated states
 */
static long unique_state(const double complex *sv, int n, int *p_count)
{
    size_t dim = (size_t)1 << n;
    long idx = -1;
    int  count = 0;
    for (size_t s = 0; s < dim; s++) {
        double pr = creal(sv[s]) * creal(sv[s]) + cimag(sv[s]) * cimag(sv[s]);
        if (pr > 1e-9) { count++; if (pr > 0.5) idx = (long)s; }
    }
    *p_count = count;
    return idx;
}

/* Decode the three sub-registers out of a basis-state index. */
static uint32_t get_reg  (long s) { return (uint32_t)( s        & 0x1F); }
static uint32_t get_carry(long s) { return (uint32_t)((s >> 5)  & 0x1F); }
static uint32_t get_flag (long s) { return (uint32_t)((s >> 10) & 0x1 ); }


/*
 * run_cmp — One comparison in isolation: preset the register, compare against a bound, decode.
 * Inputs : is_leq, regval, bound
 * Outputs: *out_reg, *out_carry, *out_flag, *out_count; gate count
 */
static int run_cmp(int is_leq, uint32_t regval, uint32_t bound,
                   uint32_t *out_reg, uint32_t *out_carry, uint32_t *out_flag,
                   int *out_count, size_t *out_gates)
{
    QkCircuit *qc = qk_circuit_new(NQ, 0);

    /* preset the register to regval (flag and carries start |0>) */
    for (uint32_t b = 0; b < M; b++)
        if ((regval >> b) & 1u) { uint32_t q[1] = { REG_Q[b] }; qk_circuit_gate(qc, QkGate_X, q, NULL); }

    if (is_leq) emit_flag_leq(qc, REG_Q, M, CARRY_Q, bound, FLAG_Q);
    else        emit_flag_geq(qc, REG_Q, M, CARRY_Q, bound, FLAG_Q);

    if (out_gates) *out_gates = qk_circuit_num_instructions(qc);

    double complex *sv = sv_alloc(NQ);
    int ng = simulate_circuit(qc, sv, NQ);

    int  count = 0;
    long s     = unique_state(sv, NQ, &count);
    *out_count = count;
    *out_reg   = (s < 0) ? 0xFFFFFFFFu : get_reg(s);
    *out_carry = (s < 0) ? 0xFFFFFFFFu : get_carry(s);
    *out_flag  = (s < 0) ? 0xFFFFFFFFu : get_flag(s);

    free(sv);
    qk_circuit_free(qc);
    return ng;
}


/*
 * sweep — Check every register value against one bound.
 * Inputs : is_leq; bound
 * Outputs: number of failures
 */
static int sweep(int is_leq, uint32_t bound)
{
    int allok = 1;
    for (uint32_t r = 0; r < (1u << M); r++) {
        uint32_t rr, cc, ff; int cnt;
        int ng = run_cmp(is_leq, r, bound, &rr, &cc, &ff, &cnt, NULL);
        uint32_t want = is_leq ? (uint32_t)(r <= bound) : (uint32_t)(r >= bound);
        int ok = (ng >= 0) && (cnt == 1) && (rr == r) && (cc == 0) && (ff == want);
        if (!ok) {
            allok = 0;
            printf("    (debug %s bound=%u reg=%u → flag=%u (want %u) reg'=%u carry=%u count=%d)\n",
                   is_leq ? "<=" : ">=", bound, r, ff, want, rr, cc, cnt);
        }
    }
    return allok;
}

/*
 * main — Run the test cases.
 * Outputs: exit code: 0 all passed
 */
int main(void)
{
    printf("Comparator / feasibility-flag unit tests\n");
    printf("================================================\n");

    /* ── A. Explicit boundary cases for cost ≤ W (W-1, W, W+1, 0, max), W = 14. ─ */
    printf("\n  A) cost <= W boundary cases (W=14, M=5):\n");
    struct { uint32_t reg; uint32_t want; const char *lbl; } bc[] = {
        {  0, 1, "0  <= 14  -> feasible" },
        { 13, 1, "13 <= 14  -> feasible (W-1)" },
        { 14, 1, "14 <= 14  -> feasible (W)" },
        { 15, 0, "15 <= 14  -> infeasible (W+1)" },
        { 31, 0, "31 <= 14  -> infeasible (max)" },
    };
    size_t gates = 0;
    for (size_t i = 0; i < sizeof bc / sizeof bc[0]; i++) {
        uint32_t rr, cc, ff; int cnt;
        run_cmp(1, bc[i].reg, 14, &rr, &cc, &ff, &cnt, (i == 0) ? &gates : NULL);
        int ok = (cnt == 1) && (rr == bc[i].reg) && (cc == 0) && (ff == bc[i].want);
        if (!ok)
            printf("    (debug reg=%u -> flag=%u carry=%u reg'=%u count=%d, want flag=%u)\n",
                   bc[i].reg, ff, cc, rr, cnt, bc[i].want);
        check(bc[i].lbl, ok);
    }
    printf("     (each comparator compiles to %zu circuit instructions)\n", gates);

    /* ── B. Exhaustive sweep: flag == (reg ≤ W) over all 32 reg values, several W.
     *      Covers 0, mid, the budget boundary, and the all-ones top (carry out of
     *      the MSB).  W = 14 is the N=4 instance budget Σ[2,3,4,5]. ───────────── */
    printf("\n  B) exhaustive cost <= W sweep (flag == (reg <= W) for all 32 reg):\n");
    uint32_t Ws[] = { 0, 1, 7, 13, 14, 15, 31 };
    for (size_t i = 0; i < sizeof Ws / sizeof Ws[0]; i++) {
        char l[72];
        snprintf(l, sizeof l, "<= W=%-2u : flag == (reg <= W) for all reg in [0,31]", Ws[i]);
        check(l, sweep(1, Ws[i]));
    }

    /* ── C. Exhaustive sweep: flag == (reg ≥ T) over all 32 reg values, several T.
     *      T = 19 is the N=4 instance value sum Σ[3,4,5,7]; T = 1 is the tightest
     *      threshold the Dürr–Høyer loop can pose (T ≥ best+1 ≥ 1). ──────────── */
    printf("\n  C) exhaustive value >= T sweep (flag == (reg >= T) for all 32 reg):\n");
    uint32_t Ts[] = { 1, 7, 13, 14, 15, 19, 31 };
    for (size_t i = 0; i < sizeof Ts / sizeof Ts[0]; i++) {
        char l[72];
        snprintf(l, sizeof l, ">= T=%-2u : flag == (reg >= T) for all reg in [0,31]", Ts[i]);
        check(l, sweep(0, Ts[i]));
    }

    printf("================================================\n");
    printf("%s\n", g_fail ? "SOME COMPARATOR TESTS FAILED"
                          : "ALL COMPARATOR TESTS PASSED");
    return g_fail;
}
