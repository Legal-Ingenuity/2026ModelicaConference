#!/usr/bin/env bash
# build_qiskit.sh — build the Qiskit C API into qiskit/dist/c/ for ModKnapsack, both forms:
#
#   qiskit/dist/c/include/qiskit.h   the header
#   qiskit/dist/c/lib/libqiskit.so   the shared library (the C self-tests link it)
#   qiskit/dist/c/lib/libqiskit.a    a static library, libpython inside (the Modelica model
#                                    links it through Resources/Library/linux64; see README.md)
#
# The Qiskit C API at the validated commit still needs libpython (pyo3 inside its crates). Linked
# against a Python whose static libpython is position-independent, the shared library needs no
# libpython at run time and the static one carries it, so a simulation (an OpenModelica run or a
# Modelon Impact FMU) needs no Python and no LD_LIBRARY_PATH.
#
# Usage, from the project root, with qiskit/ cloned and checked out (README.md):
#   ./build_qiskit.sh [python]       python: an interpreter with a static, -fPIC libpython
#                                    (default: python3; in Modelon Impact /usr/local/bin/python3.11)
#
# Needs cargo (rustup) and make on PATH. Copyright (c) 2026 Mark Myers. MIT License (see LICENSE).
set -euo pipefail

PY="${1:-python3}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
Q="$ROOT/qiskit"
[ -f "$Q/Makefile" ] || { echo "no Qiskit clone in $Q (README.md, Build and test)" >&2; exit 1; }
command -v cargo >/dev/null || { echo "cargo not found: install Rust with rustup first" >&2; exit 1; }

# the interpreter's static libpython: where it is and how it was built
read -r VER LIBPL LIBA CFLAGS < <("$PY" -c '
import sysconfig as s
v = s.get_config_var
print(v("VERSION"), v("LIBPL"), "libpython%s.a" % v("VERSION"), "PIC" if "-fPIC" in (v("CFLAGS") or "") + (v("CCSHARED") or "") else "nopic")')
if [ ! -f "$LIBPL/$LIBA" ]; then
    echo "$PY has no static $LIBA in $LIBPL; use an interpreter that has one" >&2; exit 1
fi
echo "Python $VER, static $LIBPL/$LIBA ($CFLAGS)"

# pyo3: link that static library (its default search path is LIBDIR, which may not hold it)
CFG="$Q/target/pyo3-static.cfg"
mkdir -p "$Q/target"
cat > "$CFG" <<EOF
implementation=CPython
version=$VER
shared=false
abi3=false
lib_name=python$VER
lib_dir=$LIBPL
executable=$(command -v "$PY")
pointer_width=64
build_flags=
suppress_build_script_link_lines=false
EOF
export PYO3_PYTHON="$(command -v "$PY")" PYO3_CONFIG_FILE="$CFG"

cd "$Q"
make c                                                   # header + libqiskit.so
cargo rustc -p qiskit-cext --release --crate-type staticlib   # target/release/libqiskit_cext.a

# libpython is already inside: rustc bundles a static native library into a staticlib. What is
# left to link is -ldl -lm -lpthread, which both Modelica tools link anyway.
cp target/release/libqiskit_cext.a dist/c/lib/libqiskit.a
# (grep reads all of nm's output: grep -q would stop early and fail the pipe under pipefail)
nm dist/c/lib/libqiskit.a 2>/dev/null | grep " T Py_Initialize$" >/dev/null \
    || { echo "libqiskit.a does not contain libpython: check that $PY has a static libpython" >&2; exit 1; }
ls -l dist/c/include/qiskit.h dist/c/lib/libqiskit.so dist/c/lib/libqiskit.a
