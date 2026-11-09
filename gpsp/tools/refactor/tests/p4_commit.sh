#!/usr/bin/env bash
# p4_commit.sh -- a REAL P4 step to build: split.py applied to REV's main_psp.c with a
# cutlist, written as a commit object whose parent is REV (no branch, no index, no
# working tree touched: hash-object + mktree + commit-tree).  flavours.sh --commit can
# then build it and gate it --against REV's run like any P step.
#
#   p4_commit.sh --repo REPO --base REV --cutlist CUTLIST --out DIR
#   -> prints the new commit id on the last line; DIR/split holds split.py's output
# The objects are unreferenced (git gc removes them in time).  Run from WSL or Linux.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
REPO= BASE= CUT= OUT=
while [ $# -gt 0 ]; do
  case "$1" in
    --repo) REPO=$2; shift 2 ;; --base) BASE=$2; shift 2 ;; --cutlist) CUT=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;; *) echo "p4_commit: unknown arg $1" >&2; exit 2 ;;
  esac
done
[ -n "$REPO" ] && [ -n "$BASE" ] && [ -f "$CUT" ] && [ -n "$OUT" ] || { echo "p4_commit: --repo --base --cutlist --out" >&2; exit 2; }
G() {   # git that also works on a Windows worktree from WSL (flavours.sh's rule)
  if git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1; then git -C "$REPO" "$@"
  else git.exe -C "$(wslpath -w "$REPO")" "$@" | tr -d '\r'; fi
}
mkdir -p "$OUT"; rm -rf "$OUT/split"
REV=$(G rev-parse --verify "$BASE^{commit}") || { echo "p4_commit: no commit $BASE" >&2; exit 2; }
G show "$REV:gpsp/psp/main_psp.c" > "$OUT/main_psp.base.c" || exit 2
python3 "$HERE/split.py" split --src "$OUT/main_psp.base.c" --cutlist "$CUT" --out "$OUT/split" > "$OUT/split.log" 2>&1 ||
  { tail -5 "$OUT/split.log" >&2; echo "p4_commit: split.py split failed" >&2; exit 1; }
python3 "$HERE/split.py" inverse --split "$OUT/split" -o "$OUT/inverse.c" > /dev/null 2>&1
cmp -s "$OUT/main_psp.base.c" "$OUT/inverse.c" || { echo "p4_commit: split.py inverse is not the base" >&2; exit 1; }
blob() { G hash-object -w --no-filters --stdin < "$1"; }
FE=""
for f in "$OUT/split/psp/fe/"*; do
  [ -f "$f" ] || continue
  FE="${FE}100644 blob $(blob "$f")	$(basename "$f")
"
done
[ -n "$FE" ] || { echo "p4_commit: split produced no psp/fe/ fragment" >&2; exit 1; }
FE_TREE=$(printf '%s' "$FE" | G mktree)
MAIN=$(blob "$OUT/split/psp/main_psp.c")
PSP=$(G ls-tree "$REV:gpsp/psp" | awk -v m="$MAIN" -F'\t' '$2 == "main_psp.c" { split($1, a, " "); print a[1] " " a[2] " " m "\t" $2; next } $2 != "fe" { print }')
PSP_TREE=$(printf '%s\n040000 tree %s\tfe\n' "$PSP" "$FE_TREE" | G mktree)
GPSP_TREE=$(G ls-tree "$REV:gpsp" | awk -v t="$PSP_TREE" -F'\t' '$2 == "psp" { print "040000 tree " t "\t" $2; next } { print }' | G mktree)
ROOT_TREE=$(G ls-tree "$REV" | awk -v t="$GPSP_TREE" -F'\t' '$2 == "gpsp" { print "040000 tree " t "\t" $2; next } { print }' | G mktree)
NFRAG=$(ls "$OUT/split/psp/fe" | wc -l)
# deterministic: the same base + cutlist give the same commit id (a built run can be reused)
BDATE=$(G log -1 --format=%ct "$REV")
export GIT_AUTHOR_NAME=p4_commit GIT_AUTHOR_EMAIL=p4_commit@invalid GIT_AUTHOR_DATE="$BDATE +0000"
export GIT_COMMITTER_NAME=p4_commit GIT_COMMITTER_EMAIL=p4_commit@invalid GIT_COMMITTER_DATE="$BDATE +0000"
export WSLENV="GIT_AUTHOR_NAME:GIT_AUTHOR_EMAIL:GIT_AUTHOR_DATE:GIT_COMMITTER_NAME:GIT_COMMITTER_EMAIL:GIT_COMMITTER_DATE${WSLENV:+:$WSLENV}"
C=$(printf 'scratch P4: split.py with %s on %s (%s fragments)\n\nmade by gpsp/tools/refactor/tests/p4_commit.sh; not on any branch\n' \
     "$(basename "$CUT")" "${REV:0:10}" "$NFRAG" | G commit-tree "$ROOT_TREE" -p "$REV")
[ -n "$C" ] || { echo "p4_commit: commit-tree failed" >&2; exit 1; }
# the commit must hold exactly the split: main_psp.c + psp/fe/, nothing else
CH=$(G diff --name-only "$REV" "$C" | grep -vE '^gpsp/psp/(main_psp\.c|fe/.*)$')
[ -z "$CH" ] || { echo "p4_commit: the commit changes more than the split: $CH" >&2; exit 1; }
echo "p4_commit: $C = $REV + split ($NFRAG fragments in gpsp/psp/fe/)"
echo "$C"
