#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# run.sh — run this port's tests on BOARD.
#
# A dispatcher and nothing else, the sibling of the ARM ports' test/qemu.sh.
# Every board carries its own test/boards/<id>/run.sh with the same interface,
# so this file has no case statement and no list of board names: adding a board
# adds a directory, exactly as it does for board.cmake, src/, include/ and
# test/.
#
#     BOARD=<id> test/run.sh [app]   ->  test/boards/<id>/run.sh [app]
#
# Default BOARD is native.
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD="${BOARD:-native}"

SCRIPT="$HERE/boards/$BOARD/run.sh"
if [[ ! -x "$SCRIPT" ]]; then
  if [[ -f "$HERE/boards/$BOARD/board.cmake" ]]; then
    echo "run.sh: $BOARD is a board of this port but carries no run.sh" >&2
  else
    echo "run.sh: $BOARD is not a board of this port" >&2
  fi
  echo "boards with a runner:" >&2
  for d in "$HERE"/boards/*/; do
    b="$(basename "$d")"
    [[ -x "$d/run.sh" ]] && echo "  $b" >&2
  done
  exit 2
fi

exec "$SCRIPT" "$@"
