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
# The kernel is the sibling micro-os-plus-iii (the smp branch of
# micro-os-plus-iii-smp.git); an older workspace may keep it as
# micro-os-plus-iii-smp, or as a `*.git` working copy of either name.
_siblings="$(cd "$ROOT/.." && pwd)"
SMP_DIR="${UOS_SMP_DIR:-}"
if [[ -z "$SMP_DIR" ]]; then
  for _d in micro-os-plus-iii micro-os-plus-iii.git \
            micro-os-plus-iii-smp micro-os-plus-iii-smp.git; do
    if [[ -d "$_siblings/$_d" ]]; then SMP_DIR="$_siblings/$_d"; break; fi
  done
  SMP_DIR="${SMP_DIR:-$_siblings/micro-os-plus-iii}"
fi

UOS_RUN_ONLY="${1:-}" \
exec "$SMP_DIR/test_smpl/run-host.sh" "$BUILD/test"
