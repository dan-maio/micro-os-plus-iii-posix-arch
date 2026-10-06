#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# run.sh — run this board's tests.
#
# Every board has this script, with this interface:
#
#     test/boards/<id>/run.sh            the whole suite
#     test/boards/<id>/run.sh <app>      one test
#
# The board fact here is that there is no machine: the "CPUs" are host threads
# of the test process itself, so a test is simply executed. Which is why this
# script is three lines where the ARM boards' qemu.sh has to name a QEMU
# binary, a -M machine and an -smp count.
#
# Env:  BUILD=<dir>   the CMake build directory   (default: test/build)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build}"
SMP_DIR="${UOS_SMP_DIR:-$(cd "$ROOT/.." && pwd)/micro-os-plus-iii-smp}"
# A workspace may keep the sibling as a `*.git` working copy.
[ -d "$SMP_DIR" ] || SMP_DIR="$SMP_DIR.git"

UOS_RUN_ONLY="${1:-}" \
exec "$SMP_DIR/test_smpl/run-host.sh" "$BUILD/test"
