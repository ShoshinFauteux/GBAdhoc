#!/usr/bin/env bash
# flavours.sh -- T15 (builds/mainpsp-map/out/PLAN.md §5): build the release,
# Lab and harness flavours of one tree, with the commit's OWN recipe, then run
# the phase-T checks on them.  One command per commit.
#
#   flavours.sh --commit REV --out DIR [--baseline]     export REV and build it
#   flavours.sh [--src GPSP_DIR] --out DIR [--baseline] build a checkout in place
#   flavours.sh --selftest                              no PSP build; see below
#
# Options:
#   --flavours "release lab harness"   subset / order (default all three)
#   --against REFDIR    run the T-checks of each flavour against REFDIR/<flavour>
#   --no-sidecars       skip the .map relink and the -fstack-usage replay
#   --no-sde            do not pin SOURCE_DATE_EPOCH (see DETERMINISM below)
#   --keep-src          keep the per-flavour source exports (--commit mode)
#   --work DIR          where --commit exports and builds (default: see STAGING)
#   --lock FILE         re-exec under `flock FILE` (one PSP build at a time)
#
# Environment:
#   RFU_SCHED           default 1: the beta line's adapter ticks, which is what
#                       tools/release.sh sets for X.Y.Z-beta.N and what the Lab
#                       built for beta.4.  RFU_SCHED=0 for a stable-line tree.
#   FLAV_<NAME>_ENV     extra build.sh environment for one flavour, e.g.
#                       FLAV_LAB_ENV="LAYOUT_PAD_BYTES=5904 PSP_EXTRA=LAYOUT_PAD_RO_BYTES=2864"
#                       (a baseline uses none: the commit's own pads).
#
# THE ROOT vs psp MAKE SPLIT (memory: core-vs-frontend-defines).  This script
# never builds anything itself: it calls the tree's own tools/build.sh with a
# profile name.  build.sh is the one place the flag lists live: core flags go
# to the ROOT `make platform=psp1`, frontend defines to psp/Makefile, and it
# cleans both makes (psp/*.o included) before building.  The flavours are:
#   release -> build.sh release   core GPSP_PROFILE=release, -DGPSP_PLAYABLE
#   lab     -> build.sh lab       core GPSP_PROFILE=harness, PERF_RIG+telemetry, LAB_HARNESS=1
#   harness -> build.sh harness   core GPSP_PROFILE=harness, PERF_RIG+telemetry, PSP_LARGE_MEMORY=0
# Every one of them is GPSP_PLAYABLE (memory: build-flavour-playable): a bare
# `make` in psp/ is NOT one of these flavours and is never used here.
#
# BROKEN IMPORTS (memory: psp-stub-order-trap).  psp-fixup-imports prints
# "could not fixup imports, stubs out of order" and make exits 0.  build.sh's
# run() already dies on it; this script ALSO greps every saved make log, so a
# weakened build.sh cannot let it through.  Exit 4.
#
# CLEAN TREES.  --commit exports REV with `git archive` into a fresh directory
# per flavour (the same bytes a fresh checkout gives, .gitattributes eol rules
# included), so the build is clean by construction and no flavour can inherit
# another's objects.  --src builds a checkout in place; with --baseline a tree
# whose `git status --porcelain` is not empty is REFUSED (exit 3).  Either way
# workingTreeClean is recorded in the flavour's manifest and in flavours.json.
#
# STAGING.  On WSL an --out under /mnt/c (OneDrive here) is a 9p mount: a
# 3,000-file tar extraction into it failed twice in a row ("Cannot mkdir: No
# such file or directory", wslpath not seeing an existing dir).  So --commit
# exports ONCE into a native work dir (default $HOME/.cache/flavours-work/...,
# or $FLAV_WORK, or --work), copies that export per flavour, and builds there;
# only the artifacts are copied to --out.  The export is still checked file
# for file against `git ls-tree` before anything builds.
# --out itself under /mnt/c was ALSO seen to break (2026-11-05: a dir deleted
# from Windows left WSL with "mkdir: Already exists" and no build.log), so
# on WSL prefer a native --out ($HOME/...) and copy the result to Windows
# afterwards (from Windows: \\wsl$\<distro>\home\...).
#
# DETERMINISM.  The harness and Lab flavours compile `fe_evt("build stamp=%s_%s",
# __DATE__, __TIME__)` (main_psp.c:13926), so two builds a minute apart differ in
# 20 .rodata bytes.  GCC honours SOURCE_DATE_EPOCH for __DATE__/__TIME__, so this
# script pins it to REV's commit time by putting a `docker` shim first on PATH
# that adds `-e SOURCE_DATE_EPOCH=...` to build.sh's `docker run`s.  No code is
# generated differently: the stamp is a fixed-length string.  The release
# flavour compiles fe_evt out and does not contain it at all.  --no-sde turns the
# pin off (the build is then what tools/build.sh alone gives).
#
# SIDECARS (PLAN §5 T12/G3 need .su files and a link map; neither is in the
# normal build).  flavours_sidecar.sh replays the logged compile lines with
# -fstack-usage into a side directory and requires every object to be byte-
# identical to the real one, and relinks with -Wl,-Map and requires the ELF to
# be byte-identical.  So the kept ELF is the normal build's ELF.  Exit 5 if not.
#
# CHECKS (--against).  Each T tool runs if it exists in this directory, else it
# is recorded "absent".  Contract (exit 0 = pass):
#   pair tools    sect_cmp.py (T2), func_diff.py (T1):   python3 TOOL REF.elf NEW.elf
#   single tools  hotord_check.py (T5), strong_syms.py (T6), stack_depth.py (T12):
#                                                        python3 TOOL FLAVOUR_DIR
#   source tool   ci_greps.sh (T11):                     bash TOOL GPSP_SRC_DIR
# det_check.py (section-level identity, the G1 core) always runs on pairs.
#
# Exit: 0 ok, 1 build failed, 2 usage, 3 dirty tree refused, 4 broken imports,
#       5 sidecar changed a byte, 6 a check failed.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SELF="$HERE/$(basename "$0")"
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python

die()  { local rc=$1; shift; echo "flavours: FAIL: $*" >&2; exit "$rc"; }
note() { printf 'flavours: %s\n' "$*"; }

# git that works from WSL on a Windows worktree (its .git file holds a C:/ path
# WSL's git cannot follow): fall back to git.exe with a Windows path.
# The Windows path is resolved once per dir and cached: wslpath on a /mnt/c
# path was seen to fail transiently mid-run.
declare -A WINP=()
winpath() {
  local d="$1" w i
  if [ -n "${WINP[$d]:-}" ]; then printf '%s' "${WINP[$d]}"; return 0; fi
  for i in 1 2 3; do w=$(wslpath -w "$d" 2>/dev/null) && [ -n "$w" ] && break; sleep 1; done
  [ -n "$w" ] || return 1
  WINP[$d]="$w"; printf '%s' "$w"
}
G() {   # G <dir> <git args...>
  local d="$1" w; shift
  if git -C "$d" rev-parse --git-dir >/dev/null 2>&1; then git -C "$d" "$@"
  elif command -v git.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
    w=$(winpath "$d") || return 128
    git.exe -C "$w" "$@" | tr -d '\r'
  else return 128; fi
}
GB() {  # G without the CR strip: for binary output (git archive)
  local d="$1" w; shift
  if git -C "$d" rev-parse --git-dir >/dev/null 2>&1; then git -C "$d" "$@"
  elif command -v git.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
    w=$(winpath "$d") || return 128
    git.exe -C "$w" "$@"
  else return 128; fi
}

# fixup_check <file...>: nonzero if any log carries the broken-imports warning
fixup_check() {
  local f hit=0
  for f in "$@"; do
    [ -f "$f" ] || continue
    if grep -q -e "could not fixup imports" -e "stubs out of order" "$f"; then
      echo "flavours: broken-imports warning in $f:" >&2
      grep -n -e "could not fixup imports" -e "stubs out of order" "$f" | head -3 >&2
      hit=1
    fi
  done
  return $hit
}

# make_docker_shim <dir> <sde>: a `docker` that adds -e SOURCE_DATE_EPOCH to run
make_docker_shim() {
  local dir="$1" sde="$2" real
  real=$(command -v docker) || die 1 "no docker on PATH"
  mkdir -p "$dir"
  cat > "$dir/docker" <<EOF
#!/bin/sh
# generated by flavours.sh: pin SOURCE_DATE_EPOCH for every container build.sh runs
if [ "\$1" = run ]; then shift; exec "$real" run -e SOURCE_DATE_EPOCH=$sde "\$@"; fi
exec "$real" "\$@"
EOF
  chmod +x "$dir/docker"
}

profile_of() { case "$1" in release|lab|harness) echo "$1" ;; *) return 1 ;; esac; }

selftest() { exec bash "$HERE/tests/test_flavours.sh"; }

# ----------------------------------------------------------------- args ----
COMMIT=""; SRC=""; OUT=""; BASELINE=0; FLAVOURS="release lab harness"
AGAINST=""; SIDECARS=1; SDE_ON=1; KEEP_SRC=0; LOCK=""; WORK="${FLAV_WORK:-}"
ORIG_ARGS=("$@")
while [ $# -gt 0 ]; do
  case "$1" in
    --commit)      COMMIT="$2"; shift 2 ;;
    --src)         SRC="$2"; shift 2 ;;
    --out)         OUT="$2"; shift 2 ;;
    --baseline)    BASELINE=1; shift ;;
    --flavours)    FLAVOURS="$2"; shift 2 ;;
    --against)     AGAINST="$2"; shift 2 ;;
    --no-sidecars) SIDECARS=0; shift ;;
    --no-sde)      SDE_ON=0; shift ;;
    --keep-src)    KEEP_SRC=1; shift ;;
    --work)        WORK="$2"; shift 2 ;;
    --lock)        LOCK="$2"; shift 2 ;;
    --selftest)    selftest ;;
    -h|--help)     sed -n '2,75p' "$SELF"; exit 0 ;;
    *)             die 2 "unknown argument $1" ;;
  esac
done
if [ -n "$LOCK" ] && [ -z "${FLAV_LOCKED:-}" ]; then
  command -v flock >/dev/null || die 2 "--lock needs flock (run from WSL)"
  FLAV_LOCKED=1 exec flock "$LOCK" bash "$SELF" "${ORIG_ARGS[@]:-}"
fi
[ -n "$OUT" ] || die 2 "--out DIR is required"
[ -n "$COMMIT" ] && [ -n "$SRC" ] && die 2 "--commit and --src are exclusive"
for f in $FLAVOURS; do profile_of "$f" >/dev/null || die 2 "unknown flavour '$f'"; done
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
[ -z "$AGAINST" ] || AGAINST=$(cd "$AGAINST" && pwd) || die 2 "--against: no such dir"

# the top of the git repo this script lives in (gpsp/tools/refactor/..), for
# --commit.  git archive run from a SUBDIRECTORY archives only that subdirectory,
# so it must run from the top: checked, not assumed.
REPO=$(cd "$HERE/../../.." && pwd)
if [ -n "$COMMIT" ]; then
  [ -z "$(G "$REPO" rev-parse --show-prefix)" ] || die 2 "$REPO is not the top of its git repo"
fi
if [ -z "$COMMIT" ]; then
  SRC="${SRC:-$(cd "$HERE/../.." && pwd)}"
  SRC=$(cd "$SRC" && pwd) || die 2 "--src: no such dir"
  [ -f "$SRC/tools/build.sh" ] || die 2 "$SRC is not a gpsp/ tree (no tools/build.sh)"
  REV=$(G "$SRC" rev-parse HEAD) || die 2 "$SRC is not in a git checkout"
  TREE=$(G "$SRC" rev-parse 'HEAD^{tree}')
  DIRTY=$(G "$SRC" status --porcelain -- . | head -5)
  if [ -n "$DIRTY" ]; then
    CLEAN=false
    [ "$BASELINE" = 1 ] && { printf '%s\n' "$DIRTY" >&2; die 3 "--baseline refuses a dirty tree ($SRC): git status --porcelain is not empty"; }
    note "WARNING: dirty tree; the artifacts are not reproducible from $REV"
  else CLEAN=true; fi
  SDE=$(G "$SRC" log -1 --format=%ct HEAD)
else
  REV=$(G "$REPO" rev-parse --verify "$COMMIT^{commit}") || die 2 "no such commit $COMMIT"
  TREE=$(G "$REPO" rev-parse "$REV^{tree}")
  CLEAN=true
  SDE=$(G "$REPO" log -1 --format=%ct "$REV")
fi
[ "$SDE_ON" = 1 ] || SDE=none
export RFU_SCHED="${RFU_SCHED:-1}"
note "source $REV (tree $TREE) clean=$CLEAN  SOURCE_DATE_EPOCH=$SDE  RFU_SCHED=$RFU_SCHED"
note "flavours: $FLAVOURS  ->  $OUT"

if [ -n "$COMMIT" ]; then
  if [ -z "$WORK" ]; then
    case "$OUT" in
      /mnt/*) WORK="$HOME/.cache/flavours-work/$(basename "$OUT")-$$" ;;
      *)      WORK="$OUT/.work" ;;
    esac
  fi
  rm -rf "$WORK"; mkdir -p "$WORK" || die 1 "cannot create work dir $WORK"
  WORK=$(cd "$WORK" && pwd)
  note "work dir $WORK"
  # one export, verified file for file; each flavour builds a copy of it
  EXP="$WORK/export"; mkdir -p "$EXP"
  GB "$REPO" archive --format=tar --prefix=gpsp/ "$REV:gpsp" > "$WORK/export.tar" || die 1 "git archive $REV failed"
  tar -x -C "$EXP" -f "$WORK/export.tar" || die 1 "untar of $REV:gpsp failed"
  rm -f "$WORK/export.tar"
  NWANT=$(G "$REPO" ls-tree -r --name-only "$REV:gpsp" | wc -l)
  NGOT=$(find "$EXP/gpsp" -type f -o -type l | wc -l)
  [ "$NWANT" -gt 0 ] && [ "$NGOT" -eq "$NWANT" ] || die 1 "export of $REV:gpsp has $NGOT files, the tree has $NWANT"
  note "exported $REV:gpsp ($NGOT files)"
fi

SHIM="${WORK:-$OUT}/.shim"
if [ "$SDE" != none ]; then make_docker_shim "$SHIM" "$SDE"; fi

RESULTS=()
for FL in $FLAVOURS; do
  FD="$OUT/$FL"
  # a flavour dir is always rebuilt from empty; stale files would mix runs
  rm -rf "$FD" 2>/dev/null; [ ! -e "$FD" ] || die 1 "cannot clear $FD (root-owned files from an older run? remove them as root)"
  mkdir -p "$FD/logs"
  if [ -n "$COMMIT" ]; then
    BSRC="$WORK/src-$FL"; rm -rf "$BSRC"; mkdir -p "$BSRC"
    cp -a "$EXP/gpsp" "$BSRC/" || die 1 "copy of the export failed"
    BSRC="$BSRC/gpsp"
    NGOT=$(find "$BSRC" -type f -o -type l | wc -l)
    [ "$NGOT" -eq "$NWANT" ] || die 1 "flavour copy has $NGOT files, the tree has $NWANT"
  else
    BSRC="$SRC"
  fi
  [ -f "$BSRC/tools/build.sh" ] || die 1 "$BSRC/tools/build.sh missing"
  VAR="FLAV_$(echo "$FL" | tr a-z A-Z)_ENV"; XENV="${!VAR:-}"
  note "[$FL] tools/build.sh $FL ${XENV:+($XENV)}"
  T0=$(date +%s)
  (
    [ "$SDE" != none ] && export PATH="$SHIM:$PATH"
    export GPSP_BUILD_COMMIT="$REV" GPSP_BUILD_TREE="$TREE"
    if [ "$CLEAN" = true ]; then export GPSP_BUILD_DIRTY=0; else export GPSP_BUILD_DIRTY=dirty; fi
    # shellcheck disable=SC2086
    [ -n "$XENV" ] && export $XENV
    bash "$BSRC/tools/build.sh" "$FL" --out "$FD"
  ) > "$FD/build.log" 2>&1
  RC=$?
  cp "$BSRC"/.build-*.log "$FD/logs/" 2>/dev/null
  fixup_check "$FD/build.log" "$FD"/logs/.build-*.log || die 4 "[$FL] linked with BROKEN IMPORTS"
  if [ "$RC" -ne 0 ]; then tail -30 "$FD/build.log" >&2; die 1 "[$FL] tools/build.sh exited $RC (log $FD/build.log)"; fi
  cp "$BSRC/psp/gpsp_adhoc.elf" "$FD/" || die 1 "[$FL] no gpsp_adhoc.elf"
  T1=$(date +%s)

  SIDE="skipped"
  if [ "$SIDECARS" = 1 ]; then
    note "[$FL] sidecars (.su replay, map relink)"
    W_SRC="$BSRC" W_FD="$FD" W_HERE="$HERE"
    docker run --rm -e SOURCE_DATE_EPOCH="$SDE" -e SIDECAR_JOBS="${SIDECAR_JOBS:-6}" \
      -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
      -v "$W_SRC":/build -v "$W_FD":/out -v "$W_HERE":/tools:ro -w /build pspdev/pspdev \
      bash /tools/flavours_sidecar.sh all > "$FD/sidecar.log" 2>&1
    SRC_RC=$?
    cat "$FD/sidecar.log" | sed "s/^/flavours: [$FL] /"
    [ "$SRC_RC" -eq 0 ] || die 5 "[$FL] sidecar: -fstack-usage or the map relink changed a byte, or failed (see $FD/sidecar.log, $FD/sidecar/objects.txt)"
    SIDE="ok"
  fi
  # SOURCE_DATE_EPOCH really reached the compiler: the harness/lab stamp is there
  STAMP="n/a"
  if [ "$SDE" != none ] && [ "$FL" != release ]; then
    # __DATE__ and __TIME__ are two separate literals; GCC renders them in UTC
    WD=$(LC_ALL=C date -u -d "@$SDE" '+%b %e %Y' 2>/dev/null)
    WT=$(LC_ALL=C date -u -d "@$SDE" '+%H:%M:%S' 2>/dev/null)
    if [ -n "$WD" ] && grep -aqF "$WD" "$FD/gpsp_adhoc.elf" && grep -aqF "$WT" "$FD/gpsp_adhoc.elf"; then
      STAMP="pinned ($WD $WT)"
    else die 1 "[$FL] the build stamp '$WD' '$WT' is not in the ELF: SOURCE_DATE_EPOCH did not reach the compiler"; fi
  fi
  [ "$KEEP_SRC" = 1 ] || [ -z "$COMMIT" ] || rm -rf "$WORK/src-$FL"
  ELF_SHA=$(sha256sum "$FD/gpsp_adhoc.elf" | cut -d' ' -f1)
  cat > "$FD/flavour.json" <<EOF
{
  "flavour": "$FL",
  "buildShProfile": "$FL",
  "sourceCommit": "$REV",
  "sourceTree": "$TREE",
  "workingTreeClean": $CLEAN,
  "buildMode": "$([ -n "$COMMIT" ] && echo "git-archive export" || echo "in place: $SRC")",
  "sourceDateEpoch": "$SDE",
  "rfuSched": "$RFU_SCHED",
  "extraEnv": "$XENV",
  "elfSha256": "$ELF_SHA",
  "buildSeconds": $((T1 - T0)),
  "sidecars": "$SIDE",
  "buildStamp": "$STAMP"
}
EOF
  note "[$FL] ok  elf sha256 $ELF_SHA  ($((T1 - T0)) s)"
  RESULTS+=("$FL")
done
rm -rf "$SHIM"
if [ -n "$COMMIT" ]; then
  if [ "$KEEP_SRC" = 1 ]; then note "sources kept in $WORK/src-*"
  else rm -rf "$WORK" 2>/dev/null || note "WARNING: could not remove $WORK (root-owned build files)"; fi
fi

# ----------------------------------------------------------------- checks --
CHECK_RC=0
if [ -n "$AGAINST" ]; then
  : > "$OUT/checks.txt"
  chk() {   # chk <label> <cmd...>
    local label="$1"; shift
    if "$@" >> "$OUT/checks.txt" 2>&1; then note "check $label: PASS"; echo "== $label PASS" >> "$OUT/checks.txt"
    else note "check $label: FAIL"; echo "== $label FAIL" >> "$OUT/checks.txt"; CHECK_RC=6; fi
  }
  for FL in "${RESULTS[@]}"; do
    R="$AGAINST/$FL"; N="$OUT/$FL"
    [ -f "$R/gpsp_adhoc.elf" ] || { note "check $FL: no reference ELF in $R"; CHECK_RC=6; continue; }
    chk "$FL det_check" "$PY" "$HERE/det_check.py" "$R" "$N"
    for t in sect_cmp.py func_diff.py; do
      if [ -f "$HERE/$t" ]; then chk "$FL $t" "$PY" "$HERE/$t" "$R/gpsp_adhoc.elf" "$N/gpsp_adhoc.elf"
      else note "check $FL $t: absent"; fi
    done
    for t in hotord_check.py strong_syms.py stack_depth.py; do
      if [ -f "$HERE/$t" ]; then chk "$FL $t" "$PY" "$HERE/$t" "$N"
      else note "check $FL $t: absent"; fi
    done
  done
  if [ -f "$HERE/ci_greps.sh" ]; then
    CSRC="${SRC:-}"; [ -n "$CSRC" ] || note "check ci_greps.sh: needs --src (or --keep-src)"
    [ -n "$CSRC" ] && chk "ci_greps.sh" bash "$HERE/ci_greps.sh" "$CSRC"
  else note "check ci_greps.sh: absent"; fi
fi

{
  echo '{'
  echo "  \"sourceCommit\": \"$REV\", \"sourceTree\": \"$TREE\", \"workingTreeClean\": $CLEAN,"
  echo "  \"sourceDateEpoch\": \"$SDE\", \"rfuSched\": \"$RFU_SCHED\","
  echo "  \"flavours\": \"${RESULTS[*]}\", \"against\": \"$AGAINST\", \"checksRc\": $CHECK_RC"
  echo '}'
} > "$OUT/flavours.json"
note "done: ${RESULTS[*]}  (checks rc $CHECK_RC)"
exit $CHECK_RC
