cmake_minimum_required(VERSION 3.20)
project(ModKnapsack C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Qiskit C API paths ──────────────────────────────────────────────────────
set(QISKIT_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/qiskit/dist/c/include")
set(QISKIT_LIB_DIR     "${CMAKE_SOURCE_DIR}/qiskit/dist/c/lib")

find_library(QISKIT_LIB
    NAMES qiskit
    PATHS "${QISKIT_LIB_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

# ── knapsack shared library (loaded by OpenModelica at runtime) ─────────────
add_library(knapsack SHARED knapsack/knapsack.c)

target_include_directories(knapsack PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(knapsack PRIVATE "${QISKIT_LIB}" m)

set_target_properties(knapsack PROPERTIES
    BUILD_RPATH   "${QISKIT_LIB_DIR}"
    INSTALL_RPATH "${QISKIT_LIB_DIR}"
)

# ── knapsack_test executable (standalone C test harness) ────────────────────
# main.c forward-declares callKnapsack(); knapsack.c provides the definition.
# OpenModelica never sees main.c — it #includes knapsack.c directly.
add_executable(knapsack_test knapsack/main.c knapsack/knapsack.c)

target_include_directories(knapsack_test PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(knapsack_test PRIVATE "${QISKIT_LIB}" m)

set_target_properties(knapsack_test PROPERTIES
    BUILD_RPATH "${QISKIT_LIB_DIR}"
)

# ── gate_test executable (white-box simulator gate tests) ───────────
# gate_test.c #includes knapsack.c directly (white-box access to static sv_*),
# so knapsack.c is NOT listed as a separate source here — that would double-
# define every symbol. This mirrors OpenModelica's own #include of knapsack.c.
add_executable(gate_test knapsack/gate_test.c)

target_include_directories(gate_test PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(gate_test PRIVATE "${QISKIT_LIB}" m)

set_target_properties(gate_test PROPERTIES
    BUILD_RPATH "${QISKIT_LIB_DIR}"
)

# ── adder_test executable (white-box controlled-constant-adder tests) ─
# adder_test.c #includes knapsack.c directly (white-box access to the static
# emit_controlled_const_add / emit_carry helpers), so knapsack.c is NOT listed
# as a separate source — same single-translation-unit model as gate_test above.
add_executable(adder_test knapsack/adder_test.c)

target_include_directories(adder_test PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(adder_test PRIVATE "${QISKIT_LIB}" m)

set_target_properties(adder_test PROPERTIES
    BUILD_RPATH "${QISKIT_LIB_DIR}"
)

# ── cmp_test executable (white-box comparator / feasibility-flag tests) ─
# cmp_test.c #includes knapsack.c directly (white-box access to the static
# emit_flag_leq / emit_flag_geq / emit_compare_const helpers), so knapsack.c is
# NOT listed as a separate source — same single-translation-unit model as above.
add_executable(cmp_test knapsack/cmp_test.c)

target_include_directories(cmp_test PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(cmp_test PRIVATE "${QISKIT_LIB}" m)

set_target_properties(cmp_test PROPERTIES
    BUILD_RPATH "${QISKIT_LIB_DIR}"
)

# ── oracle_test executable (white-box Grover-oracle self-check) ──────────
# oracle_test.c #includes knapsack.c directly (white-box access to the static
# emit_oracle / emit_controlled_const_sub helpers), so knapsack.c is NOT listed as
# a separate source — same single-translation-unit model as the other tests.
add_executable(oracle_test knapsack/oracle_test.c)

target_include_directories(oracle_test PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(oracle_test PRIVATE "${QISKIT_LIB}" m)

set_target_properties(oracle_test PROPERTIES
    BUILD_RPATH "${QISKIT_LIB_DIR}"
)

# ── diffuser_test executable (white-box Grover-diffuser tests) ───────────
# diffuser_test.c #includes knapsack.c directly (white-box access to the static
# emit_diffuser / emit_cnz helpers), so knapsack.c is NOT listed as a separate
# source — same single-translation-unit model as the other tests.
add_executable(diffuser_test knapsack/diffuser_test.c)

target_include_directories(diffuser_test PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(diffuser_test PRIVATE "${QISKIT_LIB}" m)

set_target_properties(diffuser_test PROPERTIES
    BUILD_RPATH "${QISKIT_LIB_DIR}"
)

# ── solver_test executable (white-box BBHT + Dürr–Høyer solver test) ─────
# solver_test.c #includes knapsack.c directly (white-box access to the static
# grover_knapsack / durr_hoyer / grover_search helpers), so knapsack.c is NOT listed
# as a separate source — same single-translation-unit model as the other tests.
add_executable(solver_test knapsack/solver_test.c)

target_include_directories(solver_test PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(solver_test PRIVATE "${QISKIT_LIB}" m)

set_target_properties(solver_test PROPERTIES
    BUILD_RPATH "${QISKIT_LIB_DIR}"
)

# ── qasm_test executable (white-box OpenQASM-3 exporter test) ─────────────
# qasm_test.c #includes knapsack.c directly (white-box access to the static
# write_qasm / emit_* / build_grover_circuit helpers), so knapsack.c is NOT listed
# as a separate source — same single-translation-unit model as the other tests.
# Exports circuits to qasm_xcheck/*.qasm (+ our sv_* statevector); the companion
# verify_qasm.py round-trips them through Qiskit (run in the project .venv).
add_executable(qasm_test knapsack/qasm_test.c)

target_include_directories(qasm_test PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(qasm_test PRIVATE "${QISKIT_LIB}" m)

set_target_properties(qasm_test PROPERTIES
    BUILD_RPATH "${QISKIT_LIB_DIR}"
)

# ── draw_test executable (white-box QDrawDetail dispatcher test) ──────────
# draw_test.c #includes knapsack.c directly (white-box access to the static
# write_circuit_drawing / write_latex_circuit_expanded helpers), so knapsack.c is
# NOT listed as a separate source — same single-translation-unit model as above.
# Drives each detail level 1..5 and checks the artifact files land in draw_xcheck/;
# the companion draw_circuit.py renders a level-4/5 .qasm to a B&W figure (.venv).
add_executable(draw_test knapsack/draw_test.c)

target_include_directories(draw_test PRIVATE "${QISKIT_INCLUDE_DIR}")

target_link_libraries(draw_test PRIVATE "${QISKIT_LIB}" m)

set_target_properties(draw_test PROPERTIES
    BUILD_RPATH "${QISKIT_LIB_DIR}"
)
