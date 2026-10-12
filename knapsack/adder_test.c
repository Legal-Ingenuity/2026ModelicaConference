/*
 * adder_test.c — white-box tests of the controlled constant adder (emit_controlled_const_add, emit_carry). Includes knapsack.c directly.
 *
 * Modules:
 *   check                  record one pass/fail
 *   unique_state           find the single populated basis state of a statevector
 *   run_add                one controlled add in isolation
 *   run_subset             the accumulation cost = sum of chosen item costs
 *   main                   the test cases

 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-10
 */

#include "knapsack.c"   /* white-box: exposes static emit_* and simulate_circuit */

/* ── tiny test harness (same style as gate_test.c) ───────────────────────────── */
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

#define ITEMS  4
#define COST_W 5                       /* cost-register width (4 bits + 1 guard)  */
#define NQ     13                      /* 4 item + 5 cost + 4 carry               */

static const uint32_t COST_Q[COST_W]    = { 4, 5, 6, 7, 8 };   /* LSB = COST_Q[0] */
static const uint32_t CARRY_Q[COST_W-1] = { 9, 10, 11, 12 };   /* carry out of bit j */


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
static uint32_t get_items(long s) { return (uint32_t)( s        & 0xF ); }
static uint32_t get_cost (long s) { return (uint32_t)((s >> 4)  & 0x1F); }
static uint32_t get_carry(long s) { return (uint32_t)((s >> 9)  & 0xF ); }


/*
 * run_add — One controlled constant add in isolation: preset the register, add, decode.
 * Inputs : label; init_cost; constant; ctrl_on; expected
 * Outputs: gate count; checks recorded
 */
static size_t run_add(const char *label, uint32_t init_cost, uint32_t constant,
                      int ctrl_on, uint32_t expected)
{
    QkCircuit *qc = qk_circuit_new(NQ, 0);

    /* control = item[0] */
    if (ctrl_on) { uint32_t q[1] = { 0 }; qk_circuit_gate(qc, QkGate_X, q, NULL); }

    /* preset the cost register to init_cost */
    for (uint32_t b = 0; b < COST_W; b++)
        if ((init_cost >> b) & 1u) { uint32_t q[1] = { COST_Q[b] }; qk_circuit_gate(qc, QkGate_X, q, NULL); }

    emit_controlled_const_add(qc, /*ctrl=*/0, constant, COST_Q, COST_W, CARRY_Q);

    size_t ngates = qk_circuit_num_instructions(qc);

    double complex *sv = sv_alloc(NQ);
    int ng = simulate_circuit(qc, sv, NQ);

    int  count = 0;
    long s     = unique_state(sv, NQ, &count);
    int  ok = (ng >= 0) && (count == 1) && (s >= 0)
              && (get_cost(s)  == expected)
              && (get_carry(s) == 0)
              && (get_items(s) == (ctrl_on ? 1u : 0u));

    if (!ok)
        printf("    (debug %-22s init=%u +%u ctrl=%d  →  cost=%ld carry=%ld items=%ld  expected cost=%u)\n",
               label, init_cost, constant, ctrl_on,
               s < 0 ? -1L : (long)get_cost(s),
               s < 0 ? -1L : (long)get_carry(s),
               s < 0 ? -1L : (long)get_items(s), expected);

    check(label, ok);

    free(sv);
    qk_circuit_free(qc);
    return ngates;
}


/*
 * run_subset — The accumulation loop: cost register = sum of the chosen items' costs.
 * Inputs : Costs; subset: bitmask
 * Outputs: checks recorded
 */
static void run_subset(const double Costs[ITEMS], uint32_t subset)
{
    /* integer costs (the model uses small integers stored as doubles) */
    uint32_t c[ITEMS];
    uint32_t expected = 0;
    for (int i = 0; i < ITEMS; i++) {
        c[i] = (uint32_t)(Costs[i] + 0.5);
        if ((subset >> i) & 1u) expected += c[i];
    }

    QkCircuit *qc = qk_circuit_new(NQ, 0);

    /* prepare the chosen item qubits */
    for (int i = 0; i < ITEMS; i++)
        if ((subset >> i) & 1u) { uint32_t q[1] = { (uint32_t)i }; qk_circuit_gate(qc, QkGate_X, q, NULL); }

    /* accumulate: one controlled constant-add per item, shared carry scratch */
    for (int i = 0; i < ITEMS; i++)
        emit_controlled_const_add(qc, (uint32_t)i, c[i], COST_Q, COST_W, CARRY_Q);

    double complex *sv = sv_alloc(NQ);
    int ng = simulate_circuit(qc, sv, NQ);

    int  count = 0;
    long s     = unique_state(sv, NQ, &count);
    int  ok = (ng >= 0) && (count == 1) && (s >= 0)
              && (get_cost(s)  == expected)
              && (get_carry(s) == 0)
              && (get_items(s) == subset);

    char label[96];
    snprintf(label, sizeof label,
             "subset {%c%c%c%c} → cost %u",
             (subset & 1) ? '0' : '.', (subset & 2) ? '1' : '.',
             (subset & 4) ? '2' : '.', (subset & 8) ? '3' : '.', expected);
    if (!ok)
        printf("    (debug got cost=%ld carry=%ld items=%ld, wanted cost=%u items=%u, count=%d)\n",
               s < 0 ? -1L : (long)get_cost(s),  s < 0 ? -1L : (long)get_carry(s),
               s < 0 ? -1L : (long)get_items(s), expected, subset, count);
    check(label, ok);

    free(sv);
    qk_circuit_free(qc);
}

/*
 * main — Run the test cases.
 * Outputs: exit code: 0 all passed
 */
int main(void)
{
    printf("Controlled constant-adder unit tests\n");
    printf("============================================\n");

    /* ── A. The primitive in isolation (control on/off, nonzero register,
     *       full carry propagation into the guard bit). ───────────────────── */
    printf("\n  A) single controlled-add (cost += k):\n");
    size_t g = 0;
    g = run_add("0 + 5  (ctrl on)  = 5",          0,  5, 1,  5);
    run_add    ("0 + 5  (ctrl OFF) = 0",          0,  5, 0,  0);   /* control gates → no-op */
    run_add    ("3 + 5  (ctrl on)  = 8",          3,  5, 1,  8);   /* add into nonzero reg  */
    run_add    ("3 + 5  (ctrl OFF) = 3",          3,  5, 0,  3);
    run_add    ("13 + 3 (ctrl on)  = 16 (guard)", 13, 3, 1, 16);   /* carry ripples to bit4 */
    run_add    ("14 + 0 (ctrl on)  = 14",         14, 0, 1, 14);   /* adding zero is identity */
    run_add    ("0 + 14 (ctrl on)  = 14",         0, 14, 1, 14);
    printf("     (each add compiles to %zu circuit instructions)\n", g);

    /* ── B. The accumulation loop cost = Σ Costs[i]·x[i] over all 16 subsets
     *       of the N=4 model instance  Costs = [2,3,4,5]. ──────────────────── */
    printf("\n  B) accumulation Σ Costs[i]·x[i] over the N=4 instance [2,3,4,5]:\n");
    const double Costs[ITEMS] = { 2.0, 3.0, 4.0, 5.0 };
    for (uint32_t subset = 0; subset < (1u << ITEMS); subset++)
        run_subset(Costs, subset);

    printf("============================================\n");
    printf("%s\n", g_fail ? "SOME ADDER TESTS FAILED"
                          : "ALL ADDER TESTS PASSED");
    return g_fail;
}
