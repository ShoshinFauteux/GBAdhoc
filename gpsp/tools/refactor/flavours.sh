#!/usr/bin/env bash
# flavours.sh -- T15 (builds/mainpsp-map/out/PLAN.md §5): build the release,
# Lab and harness flavours of one tree, with the commit's OWN recipe, then run
# the phase-T checks on them.  One command per commit.
#
#   flavours.sh --commit REV --out DIR [--baseline]     export REV and build it
#   flavours.sh [--src GPSP_DIR] --out DIR [--baseline] build a checkout in place
#   flavours.sh --check-only --out DIR --against REFDIR  checks of an existing
#                                                       build (no build)
#   flavours.sh --selftest                              no PSP build; see below
#
# Options:
#   --flavours "release release-stable lab harness"   subset / order (default all four)
#                       release-stable = build.sh release with RFU_SCHED=0 (a plain
#                       X.Y.Z release, tools/release.sh); the layout lock is
#                       reported, not enforced there, so G1 is its only byte gate
#   --against REFDIR    run the T-checks of each flavour against REFDIR/<flavour>
#                       (refuses, exit 2, when the two sides name a different
#                       docker image, build version or SOURCE_DATE_EPOCH)
#   --phase P           ci_greps.sh phase (base|P|HW2, default base)
#   --build-version V   pin the version string (GPSP_BUILD_VERSION, build.sh:54;
#                       default: the commit's 7-char hash, so two commits differ)
#   --sde N             pin SOURCE_DATE_EPOCH to N (default: the commit time)
#   DOCKER_IMAGE=I      (environment) the image build.sh and the sidecar run;
#                       pin it by digest: pspdev/pspdev@sha256:...
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
# CHECKS (--against).  Every tool is called by its REAL interface; a tool that
# is absent, or a check that cannot run, is a FAIL (never a note):
#   G1  det_check.py REF NEW; sect_cmp.py REF NEW              (dirs)
#   G2  func_diff.py --data --strict-weak REF.elf NEW.elf
#   HO  hotord_check.py --build NEW --sizes warn, and its JSON (placement +
#       staleness) equal to REF's
#   SS  strong_syms.py check --elf --map --flavour --golden strong_syms.golden.tsv
#       (release-stable is held to the release rows)
#   ST  stack_depth.py NEW.elf --su NEW/su --resolve stack_resolve.txt, and its
#       TOTAL/unmatched lines equal to REF's
#   CI  ci_greps.sh --src <the built export> --phase P: run at build time on the
#       exact source built (the export is kept until then) -> ci_greps.txt
# Every check's verdict is a line "== <label> PASS|FAIL" in OUT/checks.txt, and
# OUT/gate.txt is the commit gate line GENERATED from checks.txt (never typed).
#
# Exit: 0 ok, 1 build failed, 2 usage (or --against pin mismatch), 3 dirty tree
#       refused, 4 broken imports, 5 sidecar changed a byte, 6 a check failed.
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
    # WSL interop was seen to fail a git.exe call transiently (2026-11-07: "no such
    # commit" for a commit the previous run had just built): retry before failing
    local i o rc
    for i in 1 2 3; do
      o=$(git.exe -C "$w" "$@" 2>/dev/null) && { printf '%s\n' "$o" | tr -d '\r'; return 0; }
      rc=$?; sleep 2
    done
    return $rc
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

profile_of() { case "$1" in release|lab|harness) echo "$1" ;; release-stable) echo release ;; *) return 1 ;; esac; }
# the RFU_SCHED a flavour builds with: release-stable is the stable line (0)
rfu_of() { case "$1" in release-stable) echo 0 ;; *) echo "${RFU_SCHED:-1}" ;; esac; }
# the strong_syms golden rows a flavour is held to
ss_flavour_of() { case "$1" in release-stable) echo release ;; *) echo "$1" ;; esac; }
DOCKER_IMAGE="${DOCKER_IMAGE:-pspdev/pspdev}"; export DOCKER_IMAGE

selftest() { exec bash "$HERE/tests/test_flavours.sh"; }

# json_get <file> <key>: the string value of "key" in a flavours.sh/build.sh json
json_get() { sed -nE "s/.*\"$2\": *\"([^\"]*)\".*/\1/p" "$1" 2>/dev/null | head -1; }

# ----------------------------------------------------------------- args ----
COMMIT=""; SRC=""; OUT=""; BASELINE=0; FLAVOURS="release release-stable lab harness"
AGAINST=""; SIDECARS=1; SDE_ON=1; KEEP_SRC=0; LOCK=""; WORK="${FLAV_WORK:-}"
BVER=""; SDE_PIN=""; PHASE=base; CHECK_ONLY=0
ORIG_ARGS=("$@")
while [ $# -gt 0 ]; do
  case "$1" in
    --commit)      COMMIT="$2"; shift 2 ;;
    --src)         SRC="$2"; shift 2 ;;
    --out)         OUT="$2"; shift 2 ;;
    --baseline)    BASELINE=1; shift ;;
    --flavours)    FLAVOURS="$2"; shift 2 ;;
    --against)     AGAINST="$2"; shift 2 ;;
    --phase)       PHASE="$2"; shift 2 ;;
    --build-version) BVER="$2"; shift 2 ;;
    --sde)         SDE_PIN="$2"; shift 2 ;;
    --check-only)  CHECK_ONLY=1; shift ;;
    --no-sidecars) SIDECARS=0; shift ;;
    --no-sde)      SDE_ON=0; shift ;;
    --keep-src)    KEEP_SRC=1; shift ;;
    --work)        WORK="$2"; shift 2 ;;
    --lock)        LOCK="$2"; shift 2 ;;
    --selftest)    selftest ;;
    -h|--help)     sed -n '2,100p' "$SELF"; exit 0 ;;
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
case "$PHASE" in base|P|HW2) ;; *) die 2 "--phase must be base, P or HW2" ;; esac
[ -z "$SDE_PIN" ] || [[ "$SDE_PIN" =~ ^[0-9]+$ ]] || die 2 "--sde must be a number (seconds since the epoch)"
[ -z "$SDE_PIN" ] || [ "$SDE_ON" = 1 ] || die 2 "--sde and --no-sde are exclusive"
[ -z "$BVER" ] || [[ "$BVER" =~ ^[A-Za-z0-9._+-]+$ ]] || die 2 "--build-version: only [A-Za-z0-9._+-]"
if [ "$CHECK_ONLY" = 1 ]; then
  [ -n "$AGAINST" ] || die 2 "--check-only needs --against"
  [ -d "$OUT" ] || die 2 "--check-only: no such dir $OUT"
fi
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
[ -z "$AGAINST" ] || AGAINST=$(cd "$AGAINST" && pwd) || die 2 "--against: no such dir"

RESULTS=(); CI_RC=""
if [ "$CHECK_ONLY" = 1 ]; then
  # an existing build: its own records say what was built
  for FL in $FLAVOURS; do [ -f "$OUT/$FL/flavour.json" ] && RESULTS+=("$FL"); done
  [ "${#RESULTS[@]}" -gt 0 ] || die 2 "--check-only: no <flavour>/flavour.json under $OUT"
  REV=$(json_get "$OUT/${RESULTS[0]}/flavour.json" sourceCommit)
  TREE=$(json_get "$OUT/${RESULTS[0]}/flavour.json" sourceTree)
  CLEAN=$(sed -nE 's/.*"workingTreeClean": *(true|false).*/\1/p' "$OUT/${RESULTS[0]}/flavour.json" | head -1)
  SDE=$(json_get "$OUT/${RESULTS[0]}/flavour.json" sourceDateEpoch)
  RFU_SCHED=$(json_get "$OUT/${RESULTS[0]}/flavour.json" rfuSched)
  note "check-only: $OUT (${RESULTS[*]}) against $AGAINST"
else
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
[ -z "$SDE_PIN" ] || SDE="$SDE_PIN"
[ "$SDE_ON" = 1 ] || SDE=none
export RFU_SCHED="${RFU_SCHED:-1}"
# the version string build.sh bakes in (libretro.c's "gpSP v1.1.0-<V>"): pinned,
# or what build.sh would take itself (the 7-char commit hash)
BUILD_VERSION="${BVER:-${REV:0:7}}"
# the compiler: image as named, its digest and id, and psp-gcc's version
IMG_DIGEST=$(docker image inspect --format '{{index .RepoDigests 0}}' "$DOCKER_IMAGE" 2>/dev/null | tr -d '\r')
IMG_ID=$(docker image inspect --format '{{.Id}}' "$DOCKER_IMAGE" 2>/dev/null | tr -d '\r')
[ -n "$IMG_ID" ] || die 1 "docker image $DOCKER_IMAGE not found locally (flavours.sh never pulls)"
PSPGCC=$(docker run --rm "$DOCKER_IMAGE" psp-gcc --version 2>/dev/null | head -1 | tr -d '\r"')
note "source $REV (tree $TREE) clean=$CLEAN  SOURCE_DATE_EPOCH=$SDE  RFU_SCHED=$RFU_SCHED (release-stable: 0)"
note "build version $BUILD_VERSION$([ -n "$BVER" ] && echo ' (pinned)')  image $DOCKER_IMAGE  id ${IMG_ID:0:19}  $PSPGCC"
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

# T11 on the exact source built: the export (--commit) or the checkout (--src),
# before the export is removed.  The verdict is read by --against (CI).
CI_SRC="${EXP:+$EXP/gpsp}"; CI_SRC="${CI_SRC:-$SRC}"
if [ -f "$HERE/ci_greps.py" ] && [ -f "$CI_SRC/psp/main_psp.c" ]; then
  "$PY" "$HERE/ci_greps.py" --src "$CI_SRC" --phase "$PHASE" > "$OUT/ci_greps.txt" 2>&1; CI_RC=$?
  note "ci_greps --phase $PHASE on the built source: rc $CI_RC ($OUT/ci_greps.txt)"
else
  CI_RC=absent; echo "ci_greps: absent (no ci_greps.py or no psp/main_psp.c in $CI_SRC)" > "$OUT/ci_greps.txt"
fi

for FL in $FLAVOURS; do
  FD="$OUT/$FL"; PROF=$(profile_of "$FL"); FRFU=$(rfu_of "$FL")
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
  VAR="FLAV_$(echo "$FL" | tr a-z- A-Z_)_ENV"; XENV="${!VAR:-}"
  note "[$FL] tools/build.sh $PROF RFU_SCHED=$FRFU ${XENV:+($XENV)}"
  T0=$(date +%s)
  (
    [ "$SDE" != none ] && export PATH="$SHIM:$PATH"
    export GPSP_BUILD_COMMIT="$REV" GPSP_BUILD_TREE="$TREE" RFU_SCHED="$FRFU"
    [ -n "$BVER" ] && export GPSP_BUILD_VERSION="$BVER"
    if [ "$CLEAN" = true ]; then export GPSP_BUILD_DIRTY=0; else export GPSP_BUILD_DIRTY=dirty; fi
    # shellcheck disable=SC2086
    [ -n "$XENV" ] && export $XENV
    bash "$BSRC/tools/build.sh" "$PROF" --out "$FD"
  ) > "$FD/build.log" 2>&1
  RC=$?
  cp "$BSRC"/.build-*.log "$FD/logs/" 2>/dev/null
  fixup_check "$FD/build.log" "$FD"/logs/.build-*.log || die 4 "[$FL] linked with BROKEN IMPORTS"
  if [ "$RC" -ne 0 ]; then tail -30 "$FD/build.log" >&2; die 1 "[$FL] tools/build.sh exited $RC (log $FD/build.log)"; fi
  cp "$BSRC/psp/gpsp_adhoc.elf" "$FD/" || die 1 "[$FL] no gpsp_adhoc.elf"
  T1=$(date +%s)
  # the layout lock line build.sh printed (enforced on release/lab, reported on variants)
  LAYOUT=$(tr -d '\r' < "$FD/build.log" | sed -nE 's/^.*layout  : //p' | tr '\n' ';' | sed -e 's/;$//' -e 's/"/'"'"'/g')
  LLOCK=$(json_get "$FD/build-manifest.json" layoutLock)

  SIDE="skipped"
  if [ "$SIDECARS" = 1 ]; then
    note "[$FL] sidecars (.su replay, map relink)"
    W_SRC="$BSRC" W_FD="$FD" W_HERE="$HERE"
    docker run --rm -e SOURCE_DATE_EPOCH="$SDE" -e SIDECAR_JOBS="${SIDECAR_JOBS:-6}" \
      -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
      -v "$W_SRC":/build -v "$W_FD":/out -v "$W_HERE":/tools:ro -w /build "$DOCKER_IMAGE" \
      bash /tools/flavours_sidecar.sh all > "$FD/sidecar.log" 2>&1
    SRC_RC=$?
    cat "$FD/sidecar.log" | sed "s/^/flavours: [$FL] /"
    [ "$SRC_RC" -eq 0 ] || die 5 "[$FL] sidecar: -fstack-usage or the map relink changed a byte, or failed (see $FD/sidecar.log, $FD/sidecar/objects.txt)"
    SIDE="ok"
  fi
  # SOURCE_DATE_EPOCH really reached the compiler: the harness/lab stamp is there
  STAMP="n/a"
  if [ "$SDE" != none ] && [ "$PROF" != release ]; then
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
  "buildShProfile": "$PROF",
  "sourceCommit": "$REV",
  "sourceTree": "$TREE",
  "workingTreeClean": $CLEAN,
  "buildMode": "$([ -n "$COMMIT" ] && echo "git-archive export" || echo "in place: $SRC")",
  "sourceDateEpoch": "$SDE",
  "buildVersion": "$BUILD_VERSION",
  "buildVersionPinned": "$([ -n "$BVER" ] && echo yes || echo no)",
  "dockerImage": "$DOCKER_IMAGE",
  "dockerDigest": "$IMG_DIGEST",
  "dockerId": "$IMG_ID",
  "pspGcc": "$PSPGCC",
  "rfuSched": "$FRFU",
  "layout": "$LAYOUT",
  "layoutLock": "$LLOCK",
  "extraEnv": "$XENV",
  "elfSha256": "$ELF_SHA",
  "buildSeconds": $((T1 - T0)),
  "sidecars": "$SIDE",
  "buildStamp": "$STAMP"
}
EOF
  note "[$FL] ok  elf sha256 $ELF_SHA  ($((T1 - T0)) s)  layout: ${LAYOUT:-none}"
  RESULTS+=("$FL")
done
rm -rf "$SHIM"
if [ -n "$COMMIT" ]; then
  if [ "$KEEP_SRC" = 1 ]; then note "sources kept in $WORK/src-*"
  else rm -rf "$WORK" 2>/dev/null || note "WARNING: could not remove $WORK (root-owned build files)"; fi
fi
fi   # CHECK_ONLY

# ----------------------------------------------------------------- checks --
CHECK_RC=0
if [ -n "$AGAINST" ]; then
  # PINS FIRST (PLAN-P-beta5 §6-8, §10-9): a G1 comparison whose two sides name a
  # different compiler image, version string or SOURCE_DATE_EPOCH is not a
  # comparison of the source: refuse it (exit 2) before any check runs.
  for FL in "${RESULTS[@]}"; do
    RJ="$AGAINST/$FL/flavour.json"; NJ="$OUT/$FL/flavour.json"
    [ -f "$RJ" ] || die 2 "--against: no $RJ (the reference has no $FL flavour)"
    for k in dockerId buildVersion sourceDateEpoch rfuSched; do
      a=$(json_get "$RJ" "$k"); b=$(json_get "$NJ" "$k")
      [ -n "$a" ] && [ -n "$b" ] || die 2 "--against [$FL]: '$k' missing in $([ -n "$a" ] && echo "$NJ" || echo "$RJ") (a build from before the pins? rebuild it)"
      [ "$a" = "$b" ] || die 2 "--against [$FL]: $k differs: reference '$a', this build '$b' (pin it: DOCKER_IMAGE=...@sha256, --build-version, --sde)"
    done
  done
  : > "$OUT/checks.txt"
  chk() {   # chk <flavour|-> <gate> <label> <cmd...>
    local fl="$1" gate="$2" label="$3"; shift 3
    echo "-- $label: $*" >> "$OUT/checks.txt"
    if "$@" >> "$OUT/checks.txt" 2>&1; then note "check $label: PASS"; echo "== $fl $gate PASS ($label)" >> "$OUT/checks.txt"
    else note "check $label: FAIL"; echo "== $fl $gate FAIL ($label)" >> "$OUT/checks.txt"; CHECK_RC=6; fi
  }
  failchk() {  # failchk <flavour|-> <gate> <why>: a check that could not run is a FAIL
    note "check $1 $2: FAIL ($3)"; echo "== $1 $2 FAIL ($3)" >> "$OUT/checks.txt"; CHECK_RC=6
  }
  need() { [ -f "$HERE/$1" ]; }
  stack_same() {   # ST: the verdict passes on NEW, and TOTAL/unmatched lines equal REF's
    local a b
    "$PY" "$HERE/stack_depth.py" "$N/gpsp_adhoc.elf" --su "$N/su" --resolve "$HERE/stack_resolve.txt" --tree 0 > "$N/stack_depth.txt" 2>&1 || { cat "$N/stack_depth.txt"; return 1; }
    "$PY" "$HERE/stack_depth.py" "$R/gpsp_adhoc.elf" --su "$R/su" --resolve "$HERE/stack_resolve.txt" --tree 0 > "$N/stack_depth.ref.txt" 2>&1
    a=$(grep -E "TOTAL|unmatched" "$N/stack_depth.ref.txt"); b=$(grep -E "TOTAL|unmatched" "$N/stack_depth.txt")
    echo "ref: $a"; echo "new: $b"; [ -n "$b" ] && [ "$a" = "$b" ]
  }
  hotord_same() {  # HO: placement passes on NEW; the report equals REF's (staleness recorded, not gated alone)
    "$PY" "$HERE/hotord_check.py" --build "$N" --sizes warn --json "$N/hotord.json" || return 1
    "$PY" "$HERE/hotord_check.py" --build "$R" --sizes warn --json "$N/hotord.ref.json" > /dev/null 2>&1
    "$PY" "$HERE/flavours_gate.py" jsoneq "$N/hotord.ref.json" "$N/hotord.json"
  }
  for FL in "${RESULTS[@]}"; do
    R="$AGAINST/$FL"; N="$OUT/$FL"
    [ -f "$R/gpsp_adhoc.elf" ] || { failchk "$FL" G1 "no reference ELF in $R"; continue; }
    if need det_check.py; then chk "$FL" G1 "$FL det_check" "$PY" "$HERE/det_check.py" "$R" "$N"; else failchk "$FL" G1 "det_check.py absent"; fi
    if need sect_cmp.py;  then chk "$FL" G1 "$FL sect_cmp" "$PY" "$HERE/sect_cmp.py" "$R" "$N"; else failchk "$FL" G1 "sect_cmp.py absent"; fi
    if need func_diff.py; then chk "$FL" G2 "$FL func_diff --data --strict-weak" "$PY" "$HERE/func_diff.py" --data --strict-weak "$R/gpsp_adhoc.elf" "$N/gpsp_adhoc.elf"
    else failchk "$FL" G2 "func_diff.py absent"; fi
    if need hotord_check.py; then
      if [ -f "$N/gpsp_adhoc.map" ]; then chk "$FL" HO "$FL hotord_check" hotord_same
      else failchk "$FL" HO "no gpsp_adhoc.map (built with --no-sidecars)"; fi
    else failchk "$FL" HO "hotord_check.py absent"; fi
    if need strong_syms.py; then
      if [ -f "$N/gpsp_adhoc.map" ]; then
        chk "$FL" SS "$FL strong_syms check" "$PY" "$HERE/strong_syms.py" check --elf "$N/gpsp_adhoc.elf" --map "$N/gpsp_adhoc.map" \
          --flavour "$(ss_flavour_of "$FL")" --golden "$HERE/strong_syms.golden.tsv"
      else failchk "$FL" SS "no gpsp_adhoc.map (built with --no-sidecars)"; fi
    else failchk "$FL" SS "strong_syms.py absent"; fi
    if need stack_depth.py; then
      if [ -d "$N/su" ] && [ -d "$R/su" ]; then chk "$FL" ST "$FL stack_depth" stack_same
      else failchk "$FL" ST "no su/ (built with --no-sidecars)"; fi
    else failchk "$FL" ST "stack_depth.py absent"; fi
  done
  # CI: the verdict of the build-time run on the exact source (check-only: the record)
  if [ -z "$CI_RC" ]; then
    CI_RC=$(json_get "$OUT/flavours.json" ciGrepsRc)
    CI_PH=$(json_get "$OUT/flavours.json" phase)
    [ "${CI_PH:-base}" = "$PHASE" ] || CI_RC="recorded for --phase ${CI_PH:-base}, not $PHASE"
  fi
  { echo "-- ci_greps --phase $PHASE (build-time, $OUT/ci_greps.txt):"; cat "$OUT/ci_greps.txt" 2>/dev/null; } >> "$OUT/checks.txt"
  case "$CI_RC" in
    0) note "check ci_greps: PASS"; echo "== - CI PASS (ci_greps --phase $PHASE)" >> "$OUT/checks.txt" ;;
    *) failchk - CI "ci_greps rc ${CI_RC:-not run}" ;;
  esac
  # the gate line, generated from checks.txt
  "$PY" "$HERE/flavours_gate.py" "$OUT/checks.txt" > "$OUT/gate.txt" || CHECK_RC=6
  note "gate: $(cat "$OUT/gate.txt")"
fi

[ "$CHECK_ONLY" = 1 ] || {
  echo '{'
  echo "  \"sourceCommit\": \"$REV\", \"sourceTree\": \"$TREE\", \"workingTreeClean\": ${CLEAN:-false},"
  echo "  \"sourceDateEpoch\": \"$SDE\", \"rfuSched\": \"$RFU_SCHED\", \"buildVersion\": \"${BUILD_VERSION:-}\","
  echo "  \"dockerImage\": \"$DOCKER_IMAGE\", \"dockerId\": \"${IMG_ID:-}\", \"ciGrepsRc\": \"${CI_RC:-}\", \"phase\": \"$PHASE\","
  echo "  \"flavours\": \"${RESULTS[*]}\", \"against\": \"$AGAINST\", \"checksRc\": $CHECK_RC"
  echo '}'
} > "$OUT/flavours.json"
note "done: ${RESULTS[*]}  (checks rc $CHECK_RC)"
exit $CHECK_RC
