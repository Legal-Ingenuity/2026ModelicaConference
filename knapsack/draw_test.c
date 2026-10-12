/*
 * draw_test.c — white-box test of the QDrawDetail dispatcher (write_circuit_drawing). Includes knapsack.c directly.
 *
 * Modules:
 *   check                  record one pass/fail
 *   slurp                  read a file into memory
 *   file_has               does a file contain a string
 *   file_size              size of a file, -1 if absent
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
#include <unistd.h>
#include "knapsack.c"

static int g_pass = 0, g_fail = 0;

/*
 * check — Record one pass/fail result.
 * Inputs : cond; msg: label
 * Outputs: counters updated; line printed
 */
static void check(int cond, const char *msg)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (cond) g_pass++; else g_fail++;
}

/*
 * slurp — Read a file into a new buffer.
 * Inputs : path
 * Outputs: buffer (caller frees), NULL if missing; *len_out
 */
static char *slurp(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf) {
        size_t r = fread(buf, 1, (size_t)n, f);
        buf[r] = '\0';
        if (len_out) *len_out = (long)r;
    }
    fclose(f);
    return buf;
}

/*
 * file_has — Whether a file contains a string.
 * Inputs : path; needle
 * Outputs: 1 or 0
 */
static int  file_has(const char *path, const char *needle)
{
    char *b = slurp(path, NULL);
    if (!b) return 0;
    int ok = strstr(b, needle) != NULL;
    free(b);
    return ok;
}

/*
 * file_size — Size of a file.
 * Inputs : path
 * Outputs: bytes, or -1 if the file does not exist
 */
static long file_size(const char *path)
{
    long n = -1;
    char *b = slurp(path, &n);
    free(b);
    return n;
}

/*
 * main — Run the test cases.
 * Outputs: exit code: 0 all passed
 */
int main(void)
{
    printf("=== QDrawDetail dispatcher test (write_circuit_drawing) ===\n");

    mkdir("draw_xcheck", 0777);
    if (chdir("draw_xcheck") != 0) { perror("chdir draw_xcheck"); return 1; }

    /* A small N=2 instance (costs {1,2}, values {2,3}, budget 3). */
    double Costs2[2]  = {1.0, 2.0};
    double Values2[2] = {2.0, 3.0};
    double W2 = 3.0;

    /* Level 0 — produces no file. */
    remove(QDRAW_TEXT_FILE);
    write_circuit_drawing(0, 2, Values2, Costs2, W2);
    check(file_size(QDRAW_TEXT_FILE) < 0, "level 0 writes nothing");

    /* Level 1 — gate-level ASCII via the Qiskit C API drawer. */
    remove(QDRAW_TEXT_FILE);
    write_circuit_drawing(1, 2, Values2, Costs2, W2);
    check(file_size(QDRAW_TEXT_FILE) > 0, "level 1 -> circuitText.txt non-empty");

    /* Level 2 — light schematic (the H.U_f.D.measure block diagram). */
    remove(LATEX_OUTPUT_FILE);
    write_circuit_drawing(2, 2, Values2, Costs2, W2);
    check(file_has(LATEX_OUTPUT_FILE, "quantikz"), "level 2 -> printCircuitFile.txt is quantikz");
    check(file_has(LATEX_OUTPUT_FILE, "U_f"),      "level 2 shows the U_f block");

    /* Level 3 — expanded schematic: register wires + the oracle pipeline. */
    remove(QDRAW_SCHEMATIC_FILE);
    write_circuit_drawing(3, 2, Values2, Costs2, W2);
    check(file_has(QDRAW_SCHEMATIC_FILE, "quantikz"),   "level 3 -> circuitSchematic.tex is quantikz");
    check(file_has(QDRAW_SCHEMATIC_FILE, "+c\\cdot x"), "level 3 shows the +c.x accumulate stage");
    check(file_has(QDRAW_SCHEMATIC_FILE, "uncompute"),  "level 3 shows the uncompute stage");
    check(file_has(QDRAW_SCHEMATIC_FILE, "\\meter"),    "level 3 measures the item register");
    check(file_has(QDRAW_SCHEMATIC_FILE, "cost"),       "level 3 labels the cost ancilla register");

    /* Level 4 — gate-level OpenQASM-3 of ONE oracle+diffuser iteration. */
    remove(QDRAW_QASM_FILE);
    write_circuit_drawing(4, 2, Values2, Costs2, W2);
    check(file_has(QDRAW_QASM_FILE, "OPENQASM 3.0"), "level 4 -> circuitFigure.qasm is OpenQASM 3");
    check(file_has(QDRAW_QASM_FILE, "stdgates.inc"), "level 4 includes stdgates.inc");
    long sz4 = file_size(QDRAW_QASM_FILE);

    /* Level 5 — full depth: same export at ceil((pi/4) sqrt(2^N)) iterations, so it
     * must contain strictly more gates (a larger file) than the level-4 one-iter. */
    remove(QDRAW_QASM_FULL_FILE);
    write_circuit_drawing(5, 2, Values2, Costs2, W2);
    check(file_has(QDRAW_QASM_FULL_FILE, "OPENQASM 3.0"), "level 5 -> circuitFigureFull.qasm is OpenQASM 3");
    long sz5 = file_size(QDRAW_QASM_FULL_FILE);
    check(sz5 > sz4, "level 5 (full depth) export is larger than level 4 (one iteration)");

    /* Guard: a circuit-building level with N > Nmax must NOT emit a qasm file
     * (the oracle/diffuser emitters only support N <= KNAPSACK_NMAX). */
    double Costs6[8]  = {1, 1, 1, 1, 1, 1, 1, 1};
    double Values6[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    remove(QDRAW_QASM_FILE);
    write_circuit_drawing(4, 8, Values6, Costs6, 3.0);
    check(file_size(QDRAW_QASM_FILE) < 0, "level 4 with N=8 (>Nmax) is guarded — no qasm emitted");

    /* ...but the schematic-only levels still work for large N (no circuit build). */
    remove(QDRAW_SCHEMATIC_FILE);
    write_circuit_drawing(3, 6, Values6, Costs6, 3.0);
    check(file_size(QDRAW_SCHEMATIC_FILE) > 0, "level 3 schematic works for N=6 (schematic-only)");

    printf("\nQDrawDetail: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("Render a B&W figure out-of-band:\n"
               "  python3 draw_circuit.py --qasm draw_xcheck/circuitFigure.qasm --pdf\n");
    return g_fail ? 1 : 0;
}
