#!/usr/bin/env bash
# pp_diff.sh -- T3 / gate G0 (builds/mainpsp-map/out/PLAN.md §0.1, §5): the
# preprocessed token stream of psp/main_psp.c (`psp-gcc -E -P`, whitespace-
# normalised) is identical between two trees, in the release, Lab and harness
# flavours.  Run from WSL (needs docker and the pspdev/pspdev image).
#
#   pp_diff.sh --ref TREE_A --new TREE_B [--overlay DIR] [options]
#   pp_diff.sh --selftest            (tests/source-split_test_pp_diff.sh)
#
# TREE_*      a gpsp directory (the one holding psp/ and Makefile); Windows
#             paths (C:/...) are converted with wslpath.
# --overlay D files under D/psp/ are bind-mounted over TREE_B/psp/ (main_psp.c
#             and the fe/ fragment dir from split.py), so a split is checked
#             without copying the tree.  Mounted read-only.
# --logs DIR  a flavours.sh run dir (DIR/<flavour>/logs/.build-eboot.log):
#             the EXACT compile line of each flavour is taken from that log.
#             REQUIRED (P0a: the old default was 01067e74's run1, whose lines
#             lack -DGBADHOC_MGIFT; a silent default compared the wrong arm).
#             The log path and each flavour's -D set are printed.  Both trees are
#             preprocessed with the SAME line, so G0 isolates the source change.
#             For a tree whose Makefile/flags differ, point --logs at a build of
#             that tree.
# --flavours  default: every one of "release release-stable lab harness" in --logs
# --extra-defs "-UX -DY=1 ..."  appended to every compile line AFTER its own -D/-U
#             (so they override): one variant arm of the -E sweep (pp_sweep.sh)
# --file F    default psp/main_psp.c (object = basename .o)
# --keep DIR  keep the .i and normalised streams there
# --image I   default $DOCKER_IMAGE or pspdev/pspdev (the id is printed); pin by digest
# --sde N     SOURCE_DATE_EPOCH for both sides (default 1793908328, 01067e74's
#             commit time, as flavours.sh pins it).  Without a pin the Lab and
#             harness streams differ in fe_evt("build stamp", __DATE__, __TIME__)
#             (main_psp.c:13926) between ANY two runs; the self-test proves it.
#             --sde '' leaves it unpinned.
#
# Normalisation: per line, runs of blanks -> one space, trimmed, empty lines
# dropped (the line view, used for the diff); the verdict is taken on the
# token view (pp_norm.py: all whitespace OUTSIDE string/char literals,
# newlines included, -> one space; literals kept byte for byte, so a changed
# blank or TAB inside a string -- a .rodata change -- differs), so a split
# that only moves a line break is identical, and anything that changes a token
# (a macro, an #if branch taken, __LINE__/__FILE__/__COUNTER__ expansions,
# an include resolving to another file) differs.
#
# Exit: 0 identical in every flavour, 1 a difference, 2 usage / tool error.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
case "${1:-}" in --selftest) shift; exec bash "$HERE/tests/source-split_test_pp_diff.sh" "$@" ;; esac

REF= NEW= OVL= LOGS= FLAVOURS= FILE=psp/main_psp.c KEEP= IMAGE=${DOCKER_IMAGE:-pspdev/pspdev} SDE=1793908328 XDEFS=
while [ $# -gt 0 ]; do
  case "$1" in
    --ref) REF=$2; shift 2 ;;
    --new) NEW=$2; shift 2 ;;
    --overlay) OVL=$2; shift 2 ;;
    --logs) LOGS=$2; shift 2 ;;
    --flavours) FLAVOURS=$2; shift 2 ;;
    --extra-defs) XDEFS=$2; shift 2 ;;
    --file) FILE=$2; shift 2 ;;
    --keep) KEEP=$2; shift 2 ;;
    --image) IMAGE=$2; shift 2 ;;
    --sde) SDE=$2; shift 2 ;;
    -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
    *) echo "pp_diff: unknown arg $1" >&2; exit 2 ;;
  esac
done
topath() { case "$1" in [A-Za-z]:[/\\]*) wslpath -a "$1" ;; *) (cd "$1" 2>/dev/null && pwd) || echo "$1" ;; esac; }
[ -n "$REF" ] && [ -n "$NEW" ] || { echo "pp_diff: --ref and --new are required" >&2; exit 2; }
REF=$(topath "$REF"); NEW=$(topath "$NEW"); [ -n "$OVL" ] && OVL=$(topath "$OVL")
[ -n "$LOGS" ] || { echo "pp_diff: --logs RUN_DIR is required (a flavours.sh build of the tree's own compile lines)" >&2; exit 2; }
LOGS=$(topath "$LOGS")
[ -d "$LOGS" ] || { echo "pp_diff: --logs $LOGS: no such dir" >&2; exit 2; }
if [ -z "$FLAVOURS" ]; then
  for fl in release release-stable lab harness; do [ -f "$LOGS/$fl/logs/.build-eboot.log" ] && FLAVOURS="$FLAVOURS $fl"; done
  FLAVOURS=${FLAVOURS# }
  [ -n "$FLAVOURS" ] || { echo "pp_diff: no <flavour>/logs/.build-eboot.log under $LOGS" >&2; exit 2; }
fi
case "$XDEFS" in *[!A-Za-z0-9_=\ .+-]*) echo "pp_diff: --extra-defs: only -D/-U words" >&2; exit 2 ;; esac
for w in $XDEFS; do case "$w" in -D?*|-U?*) ;; *) echo "pp_diff: --extra-defs: '$w' is not -DNAME[=V] or -UNAME" >&2; exit 2 ;; esac; done
for d in "$REF" "$NEW"; do [ -f "$d/$FILE" ] || { echo "pp_diff: $d/$FILE missing" >&2; exit 2; }; done
command -v docker >/dev/null || { echo "pp_diff: docker not found (run from WSL)" >&2; exit 2; }
PY=python3; command -v python3 >/dev/null || PY=python
command -v "$PY" >/dev/null || { echo "pp_diff: python3 not found (the token view is pp_norm.py)" >&2; exit 2; }
IMGID=$(docker image inspect --format '{{.Id}}' "$IMAGE" 2>/dev/null) || { echo "pp_diff: image $IMAGE missing" >&2; exit 2; }

DIR=$(dirname "$FILE"); BASE=$(basename "$FILE"); OBJ=${BASE%.c}.o
W=$(mktemp -d "${TMPDIR:-/tmp}/ppdiff.XXXXXX"); trap 'rm -rf "$W"' EXIT
# The overlay is applied INSIDE the container on a container-local copy of
# TREE_B (cp -a /src /t; cp -a /ovl/. /t), so the host trees are never written
# (no mountpoint is created in them) and every #include resolves exactly as in
# a real build of the overlaid tree (a fragment's own dir psp/fe/ first).
if [ -n "$OVL" ]; then
  [ -d "$OVL/$DIR" ] || { echo "pp_diff: overlay has no $DIR/" >&2; exit 2; }
fi
echo "pp_diff: G0 $FILE  ref=$REF  new=$NEW${OVL:+ + overlay $OVL}  image=$IMAGE ${IMGID:0:19} SOURCE_DATE_EPOCH=${SDE:-unpinned}"
echo "         compile lines from $LOGS/<flavour>/logs/.build-eboot.log${XDEFS:+  + extra defs: $XDEFS}"
RC=0
for fl in $FLAVOURS; do
  LOG="$LOGS/$fl/logs/.build-eboot.log"
  [ -f "$LOG" ] || { echo "  $fl: no compile log $LOG" >&2; RC=2; continue; }
  CMD=$(grep -m1 -- " -c -o $OBJ $BASE\$" "$LOG")
  [ -n "$CMD" ] || { echo "  $fl: no '-c -o $OBJ $BASE' line in $LOG" >&2; RC=2; continue; }
  DSET=$(printf '%s\n' "$CMD" | grep -oE ' -[DU][A-Za-z_][A-Za-z0-9_]*(=[^ ]*)?' | tr -d ' ' | tr '\n' ' ')
  echo "  [$fl] -D set: $DSET"
  # a tree whose psp/Makefile defaults MGIFT on must be compared with a compile line
  # that has -DGBADHOC_MGIFT (round 10): a log from before it would gate the MGIFT=0 arm
  for t in "$REF" "$NEW"; do
    if grep -qE '^MGIFT \?= *1' "$t/psp/Makefile" 2>/dev/null && ! printf '%s' "$CMD" | grep -q -- ' -DGBADHOC_MGIFT'; then
      echo "pp_diff: $fl: $LOG's main_psp.o line lacks -DGBADHOC_MGIFT but $t/psp/Makefile has MGIFT ?= 1: wrong logs" >&2; exit 2
    fi
  done
  PP=$(printf '%s' "$CMD" | sed -e 's/ -MMD -MP / /' -e "s# -c -o $OBJ $BASE\$#${XDEFS:+ $XDEFS} -E -P -o /out/pp.i $BASE#")
  mkdir -p "$W/$fl"
  for side in ref new; do
    mkdir -p "$W/$fl/$side"
    if [ $side = ref ] || [ -z "$OVL" ]; then
      T=$([ $side = ref ] && echo "$REF" || echo "$NEW")
      RUN=(docker run --rm ${SDE:+-e SOURCE_DATE_EPOCH=$SDE} --user "$(id -u):$(id -g)" -v "$T:/t:ro" -v "$W/$fl/$side:/out" -w "/t/$DIR" "$IMAGE" sh -c "$PP")
    else
      RUN=(docker run --rm ${SDE:+-e SOURCE_DATE_EPOCH=$SDE} --user "$(id -u):$(id -g)" -v "$NEW:/src:ro" -v "$OVL:/ovl:ro" -v "$W/$fl/$side:/out" -w / "$IMAGE"
           sh -c "set -e; cp -a /src /tmp/t; cp -a /ovl/. /tmp/t/; cd /tmp/t/$DIR; $PP")
    fi
    if ! "${RUN[@]}" > "$W/$fl/$side.err" 2>&1; then
      echo "  $fl: preprocessing $side FAILED:"; sed 's/^/      /' "$W/$fl/$side.err" | tail -12; RC=2; continue 2
    fi
    sed -e 's/[[:space:]]\+/ /g' -e 's/^ //' -e 's/ $//' -e '/^$/d' "$W/$fl/$side/pp.i" > "$W/$fl/$side.lines"
    "$PY" "$HERE/pp_norm.py" < "$W/$fl/$side/pp.i" > "$W/$fl/$side.tok" || { echo "  $fl: pp_norm.py failed on $side" >&2; RC=2; continue 2; }
    [ -s "$W/$fl/$side.tok" ] || { echo "  $fl: $side preprocessed to an EMPTY stream (not called identical)" >&2; RC=2; continue 2; }
  done
  NL=$(wc -l < "$W/$fl/ref.lines"); NT=$(wc -c < "$W/$fl/ref.tok")
  if cmp -s "$W/$fl/ref.tok" "$W/$fl/new.tok"; then
    echo "  $fl: IDENTICAL  ($NL lines, $NT token bytes; sha256 $(sha256sum < "$W/$fl/ref.tok" | cut -c1-16))"
  else
    RC=$([ $RC -eq 2 ] && echo 2 || echo 1)
    echo "  $fl: DIFFERENT  (ref $NL lines / new $(wc -l < "$W/$fl/new.lines") lines)"
    diff -u "$W/$fl/ref.lines" "$W/$fl/new.lines" | sed -n '3,40p' | sed 's/^/      /'
  fi
done
if [ -n "$KEEP" ]; then KEEP=$(topath "$KEEP"); mkdir -p "$KEEP"; cp -r "$W"/. "$KEEP"/; echo "kept in $KEEP"; fi
case $RC in 0) echo "G0 ok×$(echo $FLAVOURS | wc -w)${XDEFS:+ [$XDEFS]}" ;; 1) echo "G0 FAIL" ;; *) echo "G0 ERROR" ;; esac
exit $RC
