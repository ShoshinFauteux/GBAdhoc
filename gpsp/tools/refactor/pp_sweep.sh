#!/usr/bin/env bash
# pp_sweep.sh -- G0var, the variant -E sweep of PLAN-P-beta5 §4 (common).  The engine is
# pp_sweep.py (its docstring is the manual):
#
#   pp_sweep.sh --ref TREE_A --new TREE_B --logs RUN_DIR [--overlay DIR] [--keep DIR]
#               [--allow-prune NAME]... [--arms "A B"] [--flavours "..."] [--list]
#
# Arms are GENERATED from the tree's tools/build.sh (every switch alone and every pair,
# on release, lab and harness), their compile lines DERIVED by running
# build.sh dry + `make -n` (and checked against RUN_DIR's logs), over every file the
# step changes.  Exit 0 identical, 1 different, 2 a skipped arm/flavour/file or an
# error (escape: --allow-prune NAME), 3 identical on a chosen subset (not a gate).
# Run from WSL (docker, python3).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
args=()
topath() { case "$1" in [A-Za-z]:[/\\]*) wslpath -a "$1" ;; *) printf '%s' "$1" ;; esac; }
while [ $# -gt 0 ]; do
  case "$1" in
    --ref|--new|--logs|--overlay|--keep) args+=("$1" "$(topath "$2")"); shift 2 ;;
    -h|--help) exec python3 "$HERE/pp_sweep.py" --help ;;
    *) args+=("$1"); shift ;;
  esac
done
exec python3 "$HERE/pp_sweep.py" "${args[@]}"
