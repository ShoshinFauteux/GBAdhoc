#!/usr/bin/env bash
# repin_rehearsal.sh -- replay past re-pin merges' SOURCE changes, reversed, on
# top of the pinned layout, and report what the layout needed (code-health
# C3-6; docs/LAYOUT-PINNING.md "Rehearsal").  The measure of C-3: a change
# outside the hot block needs 0 hand edits to tools/layout/layout.lock, the
# pins or psp/layout/hot_order.txt, and 1 build.
#
#   repin_rehearsal.sh prepare BASE OUT NAME=REV...
#       For each case, REV's change (REV^1..REV, first parent) to the compiled
#       sources -- everything under gpsp/ except tools/, docs/, psp/layout/ and
#       psp/fe/README.md (the layout files and what no build compiles) --
#       reversed onto BASE, FILE BY FILE: a file goes in only if every one of
#       its hunks applies and the file exists on both sides (a file the merge
#       created or deleted is left as BASE has it: its make line is not in the
#       replay).  The result is a commit object on no branch (parent BASE),
#       written with a private index, so no checkout, index or branch moves.
#       NAME=@PATCH instead applies PATCH forward, whole: a synthetic case
#       (tools/layout/rehearsal/*.patch) that reproduces a past re-pin's size
#       effect where its own hunks no longer replay on BASE.
#       Prints one line per case: NAME SHA, the files replayed and skipped.
#       OUT keeps each case's patch and file lists.
#   repin_rehearsal.sh report LOCKREV RUNS BASETAG TAG...
#       RUNS/TAG/<profile>/ holds a build of a case's commit (build.log,
#       elf.nm: e.g. builds/health-out/build_and_hash.sh --rev SHA --tag TAG
#       --profiles "release lab", whose runs/ is RUNS) and RUNS/BASETAG/
#       <profile>/ BASE's.  For each: did it link (a pin overrun fails a strict link and
#       names the pin), the build's layoutLock, and `layout_lock.py stats`
#       against BASE: the slack each pin used, every pin that MOVED and every
#       checked value that differs, with LOCKREV's layout.lock.
#
# The rehearsal never builds by itself: tools/build.sh is the build, run by
# whatever runs the proof builds (it needs docker and a clean commit).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GPSP="$(cd "$HERE/../.." && pwd)"
TOP=$(git -C "$GPSP" rev-parse --show-toplevel)
GD=$(git -C "$GPSP" rev-parse --absolute-git-dir)
g() { git -C "$TOP" "$@"; }
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python
EXCL=(':(exclude)gpsp/tools' ':(exclude)gpsp/docs' ':(exclude)gpsp/psp/layout'
      ':(exclude)gpsp/psp/fe/README.md')

prepare() {
  local base=$1 out=$2; shift 2
  base=$(g rev-parse --verify "$base^{commit}")
  mkdir -p "$out"
  out=$(cd "$out" && pwd)
  for c in "$@"; do
    local name=${c%%=*} rev=${c#*=} d wt
    d="$out/$name"; wt="$d/wt"
    rm -rf "$d"; mkdir -p "$wt"
    g -c core.autocrlf=false archive "$base" gpsp | tar -x -C "$wt"
    : > "$d/replayed"; : > "$d/skipped"
    if [ "${rev#@}" != "$rev" ]; then
      # NAME=@PATCH: a synthetic case, applied forward and whole
      # (tools/layout/rehearsal/*.patch: a past re-pin's size effect)
      local pf=${rev#@}
      case "$pf" in /*) ;; *) pf="$PWD/$pf" ;; esac
      (cd "$wt" && GIT_CEILING_DIRECTORIES="$d" git apply "$pf")
      (cd "$wt" && GIT_CEILING_DIRECTORIES="$d" git apply --numstat "$pf") | awk '{ print $3 }' > "$d/replayed"
      rev=$base
    else
    g diff --binary "$rev^1" "$rev" -- gpsp "${EXCL[@]}" > "$d/all.patch"
    for f in $(g diff --name-only "$rev^1" "$rev" -- gpsp "${EXCL[@]}"); do
      if ! g cat-file -e "$rev^1:$f" 2>/dev/null || ! g cat-file -e "$rev:$f" 2>/dev/null \
         || [ ! -e "$wt/$f" ]; then
        echo "$f (created or deleted by the merge)" >> "$d/skipped"; continue
      fi
      g diff --binary "$rev^1" "$rev" -- "$f" > "$d/one.patch"
      # outside any repository: git apply works as patch(1) does
      if (cd "$wt" && GIT_CEILING_DIRECTORIES="$d" git apply -R --check "$d/one.patch" 2>/dev/null); then
        (cd "$wt" && GIT_CEILING_DIRECTORIES="$d" git apply -R "$d/one.patch")
        echo "$f" >> "$d/replayed"
      else
        echo "$f (a hunk does not apply on BASE)" >> "$d/skipped"
      fi
    done
    rm -f "$d/one.patch"
    fi
    # the commit: BASE's tree with the replayed files only (a whole-tree add
    # would also re-normalise line ends and drop exec bits on Windows)
    local idx="$d/index" tree sha
    GIT_INDEX_FILE=$idx g read-tree "$base"
    if [ -s "$d/replayed" ]; then
      (cd "$wt" && GIT_INDEX_FILE=$idx git --git-dir="$GD" --work-tree="$wt" add -f -- $(cat "$d/replayed"))
    fi
    tree=$(GIT_INDEX_FILE=$idx g write-tree)
    sha=$(g commit-tree "$tree" -p "$base" -m "repin rehearsal $name on ${base:0:8} (not a branch)")
    rm -rf "$wt" "$idx"
    echo "$sha" > "$d/commit"
    echo "$name $sha replayed=$(wc -l < "$d/replayed") skipped=$(wc -l < "$d/skipped")"
    sed 's/^/    replayed /' "$d/replayed"
    sed 's/^/    skipped  /' "$d/skipped"
  done
}

report() {
  local lockrev=$1 runs=$2 basetag=$3; shift 3
  local lock; lock=$(mktemp)
  g show "$lockrev:gpsp/tools/layout/layout.lock" > "$lock"
  for name in "$@"; do
    for p in release lab; do
      local b="$runs/$name/$p" r="$runs/$basetag/$p"
      [ -d "$b" ] || continue
      echo "=== $name $p"
      if grep -q "layout pin [a-z_0-9]*:" "$b/build.log" 2>/dev/null; then
        grep -o "layout pin [a-z_0-9]*: [^\"]*" "$b/build.log" | head -3 | sed 's/^/  LINK FAILED: /'
        continue
      fi
      grep -o '"layoutLock": "[^"]*"' "$b/build-manifest.json" 2>/dev/null | sed 's/^/  /' || echo "  (no manifest: the build failed; see $b/build.log)"
      [ -f "$b/elf.nm" ] || { echo "  no elf.nm"; continue; }
      "$PY" "$HERE/layout_lock.py" stats "$p" "$b/elf.nm" --lock "$lock" --ref "$r/elf.nm" \
        | awk '/MOVED|differs|MISSING|slack used|checked value/ || ($3 != $2 && NF == 4 && $1 != "layout.lock")' | sed 's/^/  /'
    done
  done
  rm -f "$lock"
}

case "${1:-}" in
  prepare) shift; [ $# -ge 3 ] || { sed -n '2,32p' "$0"; exit 2; }; prepare "$@" ;;
  report)  shift; [ $# -ge 4 ] || { sed -n '2,32p' "$0"; exit 2; }; report "$@" ;;
  *) sed -n '2,32p' "$0"; exit 2 ;;
esac
