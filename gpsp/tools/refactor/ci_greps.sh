#!/usr/bin/env bash
# ci_greps.sh -- T11 (PLAN §5): the source rules of the main_psp.c split.
#   ci_greps.sh [--src GPSP_ROOT] [--phase base|P|HW2] [--pp DIR] [--rules R1,R2,...]
# Defaults: --src = the tree this script sits in, --phase base.
#   R1 no raw sceKernelExitGame( / handoff_run( outside fe_priv.h   (phase-dependent, see ci_greps.py)
#   R2 no cold/noreturn function without an explicit section attribute
#   R3 no pwr_hold reachable from a gpsp_* hook
#   R4 no logic inside fe_evt(...) arguments
#   R5 every .c includes fe_evt.h before it tests GPSP_NO_TELEMETRY
#   R6 every static function only main() calls has noinline + section(), or is pinned (ci_greps.r6_pinned)
#   R7 no raw harness read fe_ini_get*(HARNESS_INI|p_harness ...) outside fe_priv.h (HARNESS_INT/_STR)
#   R8 core_fe_pure: core_fe.h has no #if but its guard and __cplusplus, no attribute/static/definition
#   R9 no comment calls a built-in switch (root Makefile GPSP_BUILT_IN*, LAYOUT_PIN) optional/off/opt-in
# --pp DIR adds R1e: in main_psp.<flavour>.i (psp-gcc -E, see aec_pspcc.py) every exit and
# handoff call is wrapped by pwr_release_all().  Known exceptions: ci_greps.known.
# Exit 0 green, 1 violations.  Self-test: tests/abi-exit-ci_test_ci_greps.sh
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC=$2; shift 2 ;;
    *) args+=("$1"); shift ;;
  esac
done
exec python3 "$HERE/ci_greps.py" --src "$SRC" "${args[@]}"
