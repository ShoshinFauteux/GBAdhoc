#!/usr/bin/env bash
# step_scope.sh -- what a P step may touch (adversarial review 2026-11-07), on two gpsp/ trees.
#
#   step_scope.sh ds --ref TREE_A --new TREE_B [--scope-allow GLOB]...
#       DS: every file that differs (or exists on one side only) is psp/main_psp.c or
#       under psp/fe/; GLOB (bash pattern, relative to gpsp/) adds an allowed path.
#   step_scope.sh bf --ref TREE_A --new TREE_B
#       BF: the build files are byte-identical: tools/layout/layout.lock,
#       tools/layout/lab-pads.env, psp/Makefile, psp/layout/, tools/build.sh
#       (PLAN-P-beta5 §4: build.sh checks the lock in the step's own tree, so a step
#       that edits the lock or a pad would pass its own LL).
# Exit 0 in scope / identical, 1 not, 2 usage.  flavours.sh --against runs both on the
# git-archive exports of the two commits.
set -u
MODE=${1:-}; shift || true
REF= NEW= ALLOW=()
while [ $# -gt 0 ]; do
  case "$1" in
    --ref) REF=$2; shift 2 ;; --new) NEW=$2; shift 2 ;;
    --scope-allow) ALLOW+=("$2"); shift 2 ;;
    *) echo "step_scope: unknown arg $1" >&2; exit 2 ;;
  esac
done
[ -d "$REF" ] && [ -d "$NEW" ] || { echo "step_scope: --ref and --new must be dirs" >&2; exit 2; }
REF=$(cd "$REF" && pwd); NEW=$(cd "$NEW" && pwd)
case "$MODE" in
  bf)
    rc=0
    for p in tools/layout/layout.lock tools/layout/lab-pads.env psp/Makefile psp/layout tools/build.sh; do
      if [ -e "$REF/$p" ] || [ -e "$NEW/$p" ]; then
        if diff -r "$REF/$p" "$NEW/$p" > /dev/null 2>&1; then echo "  same     $p"; else echo "  DIFFERS  $p"; rc=1; fi
      else echo "  absent   $p (both sides)"; fi
    done
    echo "BF $([ $rc = 0 ] && echo ok || echo FAIL)"; exit $rc ;;
  ds)
    rc=0; n=0
    list() { (cd "$1" && find . -path ./.git -prune -o \( -type f -o -type l \) -print | sed 's#^\./##' | sort); }
    while IFS= read -r f; do
      [ -n "$f" ] || continue
      if [ -e "$REF/$f" ] && [ -e "$NEW/$f" ] && cmp -s "$REF/$f" "$NEW/$f"; then continue; fi
      n=$((n + 1)); ok=0
      case "$f" in psp/main_psp.c|psp/fe/*) ok=1 ;; esac
      for g in "${ALLOW[@]+"${ALLOW[@]}"}"; do
        # shellcheck disable=SC2053
        [[ "$f" == $g ]] && ok=1
      done
      if [ $ok = 1 ]; then echo "  in scope      $f"; else echo "  OUT OF SCOPE  $f"; rc=1; fi
    done < <(sort -u <(list "$REF") <(list "$NEW"))
    echo "DS $([ $rc = 0 ] && echo ok || echo FAIL): $n file(s) differ${ALLOW[*]:+ (also allowed: ${ALLOW[*]})}"; exit $rc ;;
  *) sed -n '2,13p' "$0"; exit 2 ;;
esac
