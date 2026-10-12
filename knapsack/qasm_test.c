/*
 * qasm_test.c — white-box test of the OpenQASM 3 exporter (write_qasm) and export of the noise-study circuits. Includes knapsack.c directly.
 *
 * Modules:
 *   check                  record one pass/fail
 *   dump_statevector       simulate a circuit and write its statevector
 *   dump_qasm              export a circuit as OpenQASM 3
 *   count_gates            number of instructions in a circuit
 *   layout_gctx            register layout for a problem
 *   test_kitchen_sink      one of every gate the project emits
 *   test_grover_circuit    a small complete Grover circuit
 *   export_study_circuit   the N = 2 noise-study circuit
 *   export_study_circuit_n4 the N = 4 noise-study circuit
 *   main                   the test cases

 * Disclaimer: During the preparation of this work the author used Anthropic's Claude (via Claude Code)
 * as a development and editing assistant. All algorithmic design, code, mathematical formulations and
 * text were directed, independently reviewed and verified by the author, who retains sole intellectual
 * property ownership of, and full responsibility for, the final work.
 *
 * Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
 * Last Change: 2026-10-10
 */
#include <sys/stat.h>
#include "knapsack.c"

#define OUTDIR "qasm_xcheck"

static int g_pass = 0, g_fail = 0;

/*
 * check — Record one pass/fail result.
 * Inputs : cond; msg: label
 * Outputs: counters updated; line printed
 */
static void check(int cond, const char *msg)
{
    if (cond) { printf("  ok   %s\n", msg); g_pass++; }
    else      { printf("  FAIL %s\n", msg); g_fail++; }
}

/*
 * dump_statevector — Simulate a circuit and write its statevector to a file.
 * Inputs : path, qc, nq
 * Outputs: 0 on success
 */
static int dump_statevector(const char *path, const QkCircuit *qc, int nq)
{
    size_t dim = (size_t)1 << nq;
    double complex *sv = calloc(dim, sizeof *sv);
    if (!sv) return -1;
    sv[0] = 1.0;                                  /* |0...0> */
    int rc = simulate_circuit(qc, sv, nq);        /* >=0 ok, -1 unhandled gate */
    if (rc >= 0) {
        FILE *f = fopen(path, "w");
        if (f) {
            for (size_t i = 0; i < dim; i++)
                fprintf(f, "%.17g %.17g\n", creal(sv[i]), cimag(sv[i]));
            fclose(f);
        } else {
            rc = -1;
        }
    }
    free(sv);
    return rc;
}

/*
 * dump_qasm — Export a circuit as OpenQASM 3.
 * Inputs : path, qc
 * Outputs: write_qasm's status
 */
static int dump_qasm(const char *path, const QkCircuit *qc)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int rc = write_qasm(f, qc);
    fclose(f);
    return rc;
}

/*
 * count_gates — Number of instructions in a circuit.
 * Inputs : qc
 * Outputs: count
 */
static size_t count_gates(const QkCircuit *qc)
{
    size_t n = 0, ni = qk_circuit_num_instructions(qc);
    for (size_t i = 0; i < ni; i++)
        if (qk_circuit_instruction_kind(qc, i) == QkOperationKind_Gate) n++;
    return n;
}

/*
 * layout_gctx — Register layout for a problem (wraps setup_gctx).
 * Inputs : g, N, costs, values, W
 * Outputs: g filled
 */
static void layout_gctx(gctx *g, int N, const uint32_t *costs,
                        const uint32_t *values, uint32_t W)
{
    setup_gctx(g, N, costs, values, W);
}

/*
 * test_kitchen_sink — Export and check a circuit with one of every gate the project emits.
 * Outputs: checks recorded
 */
static void test_kitchen_sink(void)
{
    printf("[1] kitchen-sink — one of each project gate (h x z cx cz ccx ccz c3x)\n");
    QkCircuit *qc = qk_circuit_new(5, 0);
    emit_h  (qc, 0);
    emit_x  (qc, 1);
    emit_z  (qc, 2);
    emit_cx (qc, 0, 1);
    emit_cz (qc, 1, 2);
    emit_ccx(qc, 0, 1, 2);
    emit_ccz(qc, 1, 2, 3);
    emit_c3x(qc, 0, 1, 2, 3);                     /* reads back as "mcx" */
    check(dump_qasm(OUTDIR "/kitchen.qasm", qc) == 0, "write_qasm handled every gate (rc=0)");
    check(dump_statevector(OUTDIR "/kitchen.sv", qc, 5) >= 0, "sv_* statevector written");
    printf("       (%zu gates on 5 qubits)\n", count_gates(qc));
    qk_circuit_free(qc);
}

/*
 * test_grover_circuit — Export and cross-check a small complete Grover circuit.
 * Outputs: checks recorded
 */
static void test_grover_circuit(void)
{
    printf("[2] real Grover circuit — N=2 instance, j=1 iteration\n");
    static const uint32_t costs[2] = {1, 2}, values[2] = {2, 3};
    gctx g; layout_gctx(&g, 2, costs, values, /*W=*/3);
    QkCircuit *qc = build_grover_circuit(&g, /*T=*/3, /*j=*/1);
    check(dump_qasm(OUTDIR "/grover.qasm", qc) == 0,
          "write_qasm handled the full oracle+diffuser (rc=0)");
    check(g.NQ <= 18, "NQ within cross-validation budget");
    check(dump_statevector(OUTDIR "/grover.sv", qc, g.NQ) >= 0, "sv_* statevector written");
    printf("       (NQ=%d, %zu gates)\n", g.NQ, count_gates(qc));
    qk_circuit_free(qc);
}


/*
 * export_study_circuit — Export the N = 2 noise-study circuit (T = 5, j = 1).
 * Outputs: files written
 */
static void export_study_circuit(void)
{
    printf("[3] noise-study circuit — N=2, M=1 (T=5), j=1 -> ideal P(success)=1\n");
    static const uint32_t costs[2] = {1, 2}, values[2] = {2, 3};
    const uint32_t W = 3, T = 5;
    gctx g; layout_gctx(&g, 2, costs, values, W);
    QkCircuit *qc = build_grover_circuit(&g, T, /*j=*/1);
    check(dump_qasm(OUTDIR "/study.qasm", qc) == 0, "study circuit exported");
    size_t ng = count_gates(qc);
    qk_circuit_free(qc);

    FILE *m = fopen(OUTDIR "/study.meta", "w");
    if (m) {
        fprintf(m, "N %d\nNQ %d\nW %u\nT %u\n", g.N, g.NQ, W, T);
        fprintf(m, "item_qubits");
        for (int i = 0; i < g.N; i++) fprintf(m, " %u", g.item[i]);
        fprintf(m, "\nmarked_masks");
        for (uint32_t x = 0; x < (1u << g.N); x++)
            if (is_marked(x, &g, T)) fprintf(m, " %u", x);
        fprintf(m, "\n");
        fclose(m);
    }
    check(m != NULL, "study metadata written (item qubits + marked set)");
    printf("       (NQ=%d, %zu gates)\n", g.NQ, ng);
}


/*
 * export_study_circuit_n4 — Export the N = 4 noise-study circuit (T = 8, j = 3).
 * Outputs: files written
 */
static void export_study_circuit_n4(void)
{
    printf("[4] HEADLINE noise circuit — N=4 baseline (T=8), j=3 -> ideal P(success)~0.96\n");
    static const uint32_t costs[4] = {2, 3, 4, 5}, values[4] = {3, 4, 5, 7};
    const uint32_t W = 6, T = 8;
    const int j = 3;
    gctx g; layout_gctx(&g, 4, costs, values, W);
    QkCircuit *qc = build_grover_circuit(&g, T, j);
    check(dump_qasm(OUTDIR "/study4.qasm", qc) == 0, "N=4 study circuit exported");
    size_t ng = count_gates(qc);
    qk_circuit_free(qc);

    FILE *m = fopen(OUTDIR "/study4.meta", "w");
    if (m) {
        fprintf(m, "N %d\nNQ %d\nW %u\nT %u\nj %d\n", g.N, g.NQ, W, T, j);
        fprintf(m, "item_qubits");
        for (int i = 0; i < g.N; i++) fprintf(m, " %u", g.item[i]);
        fprintf(m, "\nmarked_masks");
        for (uint32_t x = 0; x < (1u << g.N); x++)
            if (is_marked(x, &g, T)) fprintf(m, " %u", x);
        fprintf(m, "\n");
        fclose(m);
    }
    check(m != NULL, "N=4 study metadata written");
    printf("       (NQ=%d, %zu gates)\n", g.NQ, ng);
}

/*
 * main — Run the test cases.
 * Outputs: exit code: 0 all passed
 */
int main(void)
{
    mkdir(OUTDIR, 0777);                          /* harmless if it already exists */
    printf("=== OpenQASM-3 exporter test (write_qasm) ===\n");
    test_kitchen_sink();
    test_grover_circuit();
    export_study_circuit();
    export_study_circuit_n4();
    printf("\nC-side: %d passed, %d failed\n", g_pass, g_fail);
    printf("Out-of-band:  python3 verify_qasm.py   &&   python3 noise_study.py\n");
    return g_fail ? 1 : 0;
}
