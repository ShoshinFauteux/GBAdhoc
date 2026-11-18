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
#   --flavours "release lab harness"   subset / order (default all three).  The
#                       fourth flavour, release-stable (RFU_SCHED=0), went with the
#                       switch in code-health C-4: adapter ticks are in every build
#   --against REFDIR    run the T-checks of each flavour against REFDIR/<flavour>
#                       (refuses, exit 2, when the two sides name a different
#                       docker image, build version or SOURCE_DATE_EPOCH)
#   --ref-ancestor REV  accept a reference that is an ANCESTOR of this build's commit
#                       (a merge's other parent such as the beta tip, or P0c; PLAN-P-beta5
#                       P7), only when named: REV must be the reference's own commit
#   --phase P           ci_greps.sh phase (base|P|HW2, default base)
#   --expect-code-change  the step is MEANT to change code: an identity gate (below)
#                       whose check found a difference reads CHANGED, not FAIL
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
#   RFU_SCHED           always 1 (any caller value is ignored): a tree from before
#                       code-health C-4 needs it to build adapter ticks, a later
#                       tree's build.sh accepts it as a no-op.
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
#   ST  stack_depth.py NEW.elf --su NEW/su --resolve stack_resolve.txt, and its
#       TOTAL/unmatched lines equal to REF's
#   CI  ci_greps.sh --src <the built export> --phase P: run at build time on the
#       exact source built (the export is kept until then) -> ci_greps.txt
#   G0  pp_diff.sh on psp/main_psp.c, REF's and NEW's sources (git archive of both
#       sourceCommits), REF's compile lines, per flavour
#   G0var pp_sweep.sh: every build.sh switch alone and in pairs, on all three
#       flavours, over every file the step changes (a pruned arm is a FAIL)
#   LL  layoutLock release:ok / lab:ok on both sides (also a refusal, below)
#   CF  tools/check_flags.py (this tool tree's copy) rc 0 on both exports, same output
#   BF  layout.lock, lab-pads.env, psp/Makefile, psp/layout/, tools/build.sh
#       byte-identical between the two exports
#   DS  every file that differs between the exports is psp/main_psp.c or under
#       psp/fe/ (--scope-allow GLOB adds a pattern; it is printed in the verdict)
#   FP  frame parity (owner decision 2026-11-17): tools/layout/frame_parity.py check
#       NEW release ELF vs NEW lab ELF against the commit's frame_parity.base: no
#       per-frame function gains a release/lab code difference or (where they
#       shared one, or in the hot block) an I-cache set difference
# Every check's verdict is a line "== <flavour|-> <gate> PASS|DIFF|FAIL" in
# OUT/checks.txt, and OUT/gate.txt is the commit gate line GENERATED from it
# (never typed); flavours_gate.py fails a gate that is missing or that lacks a
# flavour it must cover.
# IDENTITY gates (G0 G0var G1 G2 HO ST BF DS) prove a step changed nothing, so
# on a commit that changes code they differ by design.  Their verdict is DIFF
# when the tool ran and found a difference (exit 1, no crash in its output) and
# FAIL when it could not run or broke (any other exit; HO's placement check and
# ST's stack verdict on the new build are invariants inside those gates and
# exit 2).  Without --expect-code-change a DIFF fails the gate as before; with
# it, the gate line says `G1 CHANGED rel/lab/har` and only FAILs, MISSING gates
# and the invariant gates (SS LL CF CI FP) can fail it.
#
# HERMETIC BUILDS (adversarial review 2026-11-07).  build.sh reads about 25
# environment variables (ME_LOWLAT, MGIFT, REWIND, CHEATS, INPUT_FRESH,
# LAYOUT_PIN, LAYOUT_LOCK, CORE_EXTRA, ...).  A variable left over in the caller's
# shell used to reach it unrecorded: with ME_LOWLAT=0 exported, a change under
# `#if PLAY_ME_LOWLAT == 2` passed every gate.  build.sh now runs under `env -i`
# with a fixed PATH, HOME, the docker client variables, and ONLY what this script
# sets (DOCKER_IMAGE, GPSP_BUILD_*, RFU_SCHED) plus FLAV_<NAME>_ENV.  Everything
# passed is recorded in flavour.json "buildEnv"; caller variables build.sh would
# have read are listed in "callerEnvDropped".
#
# --against REFUSES (exit 2), before any check, unless on both sides and in every
# one of the three flavours: hermetic is yes; dockerId, buildVersion,
# sourceDateEpoch, rfuSched, buildEnv (less GPSP_BUILD_COMMIT/TREE) and extraEnv
# are equal; workingTreeClean is true; layoutLock is release:ok on release and
# lab:ok on lab; and the reference is this build's commit, its first parent, a
# commit with the same gpsp/ tree (gpspTree), or the ancestor --ref-ancestor names
# (it must be the reference's commit and an ancestor; anything else is refused).
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
# path was seen to fail transiently mid-run.  THE CACHE IS FILLED IN THIS SHELL
# (winpath_prime, P-review 2026-11-08, F5): winpath() runs inside `$(...)`, a
# subshell, so its own WINP[$d]= assignment never reached the script and every
# git.exe call re-ran wslpath; one transient failure then failed G silently
# ("no such commit", or a failed export).  The dirs G is called on (REPO, SRC)
# are primed right after they are known; a subshell reads the parent's cache.
declare -A WINP=()
winpath() {
  local d="$1" w i
  if [ -n "${WINP[$d]:-}" ]; then printf '%s' "${WINP[$d]}"; return 0; fi
  for i in 1 2 3; do w=$(wslpath -w "$d" 2>/dev/null) && [ -n "$w" ] && break; sleep 1; done
  [ -n "$w" ] || return 1
  WINP[$d]="$w"; printf '%s' "$w"
}
winpath_prime() {   # winpath_prime <dir>: resolve and cache in THIS shell (never call it in $(...))
  local d="$1" w="" i
  [ -n "${WINP[$d]:-}" ] && return 0
  git -C "$d" rev-parse --git-dir >/dev/null 2>&1 && return 0      # WSL's git reads it: no git.exe
  { command -v git.exe && command -v wslpath; } >/dev/null 2>&1 || return 0
  for i in 1 2 3 4 5 6; do w=$(wslpath -w "$d" 2>/dev/null) && [ -n "$w" ] && break; w=""; sleep 2; done
  [ -n "$w" ] || die 1 "wslpath -w $d failed $i times: the repo cannot be reached through git.exe"
  WINP[$d]="$w"
}
G() {   # G <dir> <git args...>
  local d="$1" w; shift
  if git -C "$d" rev-parse --git-dir >/dev/null 2>&1; then git -C "$d" "$@"
  elif command -v git.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
    w=$(winpath "$d") || { echo "flavours: wslpath -w $d failed (3 tries; not primed?): git $1 not run" >&2; return 128; }
    # WSL interop was seen to fail a git.exe call transiently (2026-11-07: "no such
    # commit" for a commit the previous run had just built): retry before failing
    # (again 2026-11-07, two builds in one second: three tries 2 s apart were not enough)
    local i o rc
    for i in 1 2 3 4 5 6; do
      o=$(git.exe -C "$w" "$@" 2>"${TMPDIR:-/tmp}/flavours-git.$$.err") && { rm -f "${TMPDIR:-/tmp}/flavours-git.$$.err"; printf '%s\n' "$o" | tr -d '\r'; return 0; }
      rc=$?; sleep 5
    done
    echo "flavours: git.exe $* failed $i times: $(tail -2 "${TMPDIR:-/tmp}/flavours-git.$$.err" 2>/dev/null | tr -d '\r')" >&2
    rm -f "${TMPDIR:-/tmp}/flavours-git.$$.err"
    return $rc
  else return 128; fi
}
GB() {  # G without the CR strip: for binary output (git archive)
  local d="$1" w; shift
  if git -C "$d" rev-parse --git-dir >/dev/null 2>&1; then git -C "$d" "$@"
  elif command -v git.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
    w=$(winpath "$d") || { echo "flavours: wslpath -w $d failed (3 tries; not primed?): git $1 not run" >&2; return 128; }
    # retried like G (2026-11-07: both exports of one --against failed in the same second)
    local i rc t="${TMPDIR:-/tmp}/flavours-gb.$$.out"
    for i in 1 2 3 4 5 6; do
      if git.exe -C "$w" "$@" > "$t" 2>"$t.err"; then cat "$t"; rm -f "$t" "$t.err"; return 0; fi
      rc=$?; sleep 5
    done
    echo "flavours: git.exe $1 failed $i times: $(tail -2 "$t.err" | tr -d '\r')" >&2
    rm -f "$t" "$t.err"; return $rc
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
DOCKER_IMAGE="${DOCKER_IMAGE:-pspdev/pspdev}"; export DOCKER_IMAGE

selftest() { exec bash "$HERE/tests/test_flavours.sh"; }

# json_get <file> <key>: the string value of "key" in a flavours.sh/build.sh json
json_get() { sed -nE "s/.*\"$2\": *\"([^\"]*)\".*/\1/p" "$1" 2>/dev/null | head -1; }

# ----------------------------------------------------------------- args ----
COMMIT=""; SRC=""; OUT=""; BASELINE=0; FLAVOURS="release lab harness"
AGAINST=""; SIDECARS=1; SDE_ON=1; KEEP_SRC=0; LOCK=""; WORK="${FLAV_WORK:-}"
BVER=""; SDE_PIN=""; PHASE=base; CHECK_ONLY=0; REPO_ARG="${FLAV_REPO:-}"; REF_ANCESTOR=""
EXPECT_CHANGE=0
SCOPE_ALLOW=(); PRUNE_ALLOW=()
ALL_FLAVOURS="release lab harness"
ORIG_ARGS=("$@")
while [ $# -gt 0 ]; do
  case "$1" in
    --commit)      COMMIT="$2"; shift 2 ;;
    --src)         SRC="$2"; shift 2 ;;
    --out)         OUT="$2"; shift 2 ;;
    --baseline)    BASELINE=1; shift ;;
    --flavours)    FLAVOURS="$2"; shift 2 ;;
    --against)     AGAINST="$2"; shift 2 ;;
    --ref-ancestor) REF_ANCESTOR="$2"; shift 2 ;;
    --phase)       PHASE="$2"; shift 2 ;;
    --expect-code-change) EXPECT_CHANGE=1; shift ;;
    --build-version) BVER="$2"; shift 2 ;;
    --sde)         SDE_PIN="$2"; shift 2 ;;
    --check-only)  CHECK_ONLY=1; shift ;;
    --no-sidecars) SIDECARS=0; shift ;;
    --no-sde)      SDE_ON=0; shift ;;
    --keep-src)    KEEP_SRC=1; shift ;;
    --work)        WORK="$2"; shift 2 ;;
    --lock)        LOCK="$2"; shift 2 ;;
    --repo)        REPO_ARG="$2"; shift 2 ;;
    --scope-allow) SCOPE_ALLOW+=("$2"); shift 2 ;;
    --allow-prune) PRUNE_ALLOW+=("$2"); shift 2 ;;
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
[ -z "$REF_ANCESTOR" ] || [ -n "$AGAINST" ] || die 2 "--ref-ancestor needs --against"
if [ -n "$AGAINST" ]; then
  # a gate that covers one flavour is no gate (adversarial review 2026-11-07)
  [ "$(echo $FLAVOURS | tr ' ' '\n' | sort | tr '\n' ' ')" = "$(echo $ALL_FLAVOURS | tr ' ' '\n' | sort | tr '\n' ' ')" ] ||
    die 2 "--against needs all three flavours ($ALL_FLAVOURS), not '$FLAVOURS'"
fi
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
[ -z "$AGAINST" ] || AGAINST=$(cd "$AGAINST" && pwd) || die 2 "--against: no such dir"
# the top of the git repo (default: the one this script lives in, gpsp/tools/refactor/..).
# --repo / FLAV_REPO: run a snapshot of these tools against another checkout of the repo.
REPO=$(cd "${REPO_ARG:-$HERE/../../..}" && pwd) || die 2 "--repo: no such dir"
winpath_prime "$REPO"     # F5: before the first G/GB call, in this shell

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
# --commit: git archive run from a SUBDIRECTORY archives only that subdirectory,
# so REPO must be the top: checked, not assumed.
if [ -n "$COMMIT" ]; then
  [ -z "$(G "$REPO" rev-parse --show-prefix)" ] || die 2 "$REPO is not the top of its git repo"
fi
if [ -z "$COMMIT" ]; then
  SRC="${SRC:-$(cd "$HERE/../.." && pwd)}"
  SRC=$(cd "$SRC" && pwd) || die 2 "--src: no such dir"
  [ -f "$SRC/tools/build.sh" ] || die 2 "$SRC is not a gpsp/ tree (no tools/build.sh)"
  winpath_prime "$SRC"
  REV=$(G "$SRC" rev-parse HEAD) || die 2 "$SRC is not in a git checkout"
  TREE=$(G "$SRC" rev-parse 'HEAD^{tree}')
  DIRTY=$(G "$SRC" status --porcelain -- . | head -5)
  if [ -n "$DIRTY" ]; then
    CLEAN=false
    [ "$BASELINE" = 1 ] && { printf '%s\n' "$DIRTY" >&2; die 3 "--baseline refuses a dirty tree ($SRC): git status --porcelain is not empty"; }
    note "WARNING: dirty tree; the artifacts are not reproducible from $REV"
  else CLEAN=true; fi
  SDE=$(G "$SRC" log -1 --format=%ct HEAD)
  PARENT=$(G "$SRC" rev-parse --verify -q 'HEAD^1' 2>/dev/null) || PARENT=none
  GTREE=$(G "$SRC" rev-parse 'HEAD:./' 2>/dev/null) || GTREE=unknown
else
  REV=$(G "$REPO" rev-parse --verify "$COMMIT^{commit}") || die 2 "no such commit $COMMIT"
  TREE=$(G "$REPO" rev-parse "$REV^{tree}")
  CLEAN=true
  SDE=$(G "$REPO" log -1 --format=%ct "$REV")
  PARENT=$(G "$REPO" rev-parse --verify -q "$REV^1" 2>/dev/null) || PARENT=none
  GTREE=$(G "$REPO" rev-parse "$REV:gpsp" 2>/dev/null) || GTREE=unknown
fi
[ -n "$PARENT" ] || PARENT=none
[ -z "$SDE_PIN" ] || SDE="$SDE_PIN"
[ "$SDE_ON" = 1 ] || SDE=none
export RFU_SCHED=1   # see the header: a no-op for a C-4 tree
# the version string build.sh bakes in (libretro.c's "gpSP v1.1.0-<V>"): pinned,
# or what build.sh would take itself (the 7-char commit hash)
BUILD_VERSION="${BVER:-${REV:0:7}}"
# the compiler: image as named, its digest and id, and psp-gcc's version
IMG_DIGEST=$(docker image inspect --format '{{index .RepoDigests 0}}' "$DOCKER_IMAGE" 2>/dev/null | tr -d '\r')
IMG_ID=$(docker image inspect --format '{{.Id}}' "$DOCKER_IMAGE" 2>/dev/null | tr -d '\r')
[ -n "$IMG_ID" ] || die 1 "docker image $DOCKER_IMAGE not found locally (flavours.sh never pulls)"
PSPGCC=$(docker run --rm "$DOCKER_IMAGE" psp-gcc --version 2>/dev/null | head -1 | tr -d '\r"')
note "source $REV (tree $TREE) clean=$CLEAN  SOURCE_DATE_EPOCH=$SDE  RFU_SCHED=$RFU_SCHED"
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
  FD="$OUT/$FL"; PROF=$(profile_of "$FL"); FRFU=$RFU_SCHED
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
  # HERMETIC: build.sh sees only this environment (see the header)
  BENV=("DOCKER_IMAGE=$DOCKER_IMAGE" "GPSP_BUILD_COMMIT=$REV" "GPSP_BUILD_TREE=$TREE" "RFU_SCHED=$FRFU")
  [ -n "$BVER" ] && BENV+=("GPSP_BUILD_VERSION=$BVER")
  if [ "$CLEAN" = true ]; then BENV+=("GPSP_BUILD_DIRTY=0"); else BENV+=("GPSP_BUILD_DIRTY=dirty"); fi
  for w in $XENV; do
    [[ "$w" =~ ^[A-Z_][A-Z0-9_]*=[^[:space:]]*$ ]] || die 2 "$VAR: '$w' is not NAME=VALUE"
    BENV+=("$w")
  done
  for v in ${FLAV_TEST_PASSENV:-}; do [ -n "${!v+x}" ] && BENV+=("$v=${!v}"); done   # tests/test_flavours.sh only
  BENV_REC=$(printf '%s\n' "${BENV[@]}" | sort | tr '\n' ' ' | sed 's/ $//; s/"/'"'"'/g')
  # caller variables build.sh would have read, now dropped (recorded, never passed)
  DROPPED=""
  BS_NAMES=$(grep -oE '\$\{?!?[A-Z_][A-Z0-9_]*' "$BSRC/tools/build.sh" | sed -E 's/^\$\{?!?//' | sort -u)
  for v in $BS_NAMES; do
    case " $BENV_REC " in *" $v="*) continue ;; esac
    case "$v" in PATH|HOME|PWD) continue ;; esac
    [ -n "${!v+x}" ] && [ "$(declare -p "$v" 2>/dev/null | cut -c1-10)" = "declare -x" ] && DROPPED="$DROPPED $v=${!v}"
  done
  DROPPED=$(echo $DROPPED | sed 's/"/'"'"'/g')
  [ -z "$DROPPED" ] || note "[$FL] caller environment NOT passed to build.sh:$DROPPED"
  DPATH="$(dirname "$(command -v docker)"):/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
  [ "$SDE" != none ] && DPATH="$SHIM:$DPATH"
  DCLI=()
  for v in DOCKER_HOST DOCKER_CONTEXT DOCKER_CONFIG DOCKER_CERT_PATH DOCKER_TLS_VERIFY; do
    [ -n "${!v:-}" ] && DCLI+=("$v=${!v}")
  done
  T0=$(date +%s)
  env -i PATH="$DPATH" HOME="$HOME" "${DCLI[@]}" "${BENV[@]}" bash "$BSRC/tools/build.sh" "$PROF" --out "$FD" > "$FD/build.log" 2>&1
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
  "sourceParent": "$PARENT",
  "gpspTree": "$GTREE",
  "workingTreeClean": $CLEAN,
  "hermetic": "yes",
  "buildEnv": "$BENV_REC",
  "callerEnvDropped": "$DROPPED",
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
  # THE CONTRACT (adversarial review 2026-11-07): also the build environment, a
  # clean tree, the layout lock, the flavour set and the ancestry (see the header).
  [ "${#RESULTS[@]}" = 3 ] || die 2 "--against: this build has flavours '${RESULTS[*]}', all three are required ($ALL_FLAVOURS)"
  for FL in $ALL_FLAVOURS; do
    [ -f "$AGAINST/$FL/flavour.json" ] || die 2 "--against: no $AGAINST/$FL/flavour.json (the reference lacks $FL; all three are required)"
  done
  LOCK_AT=()
  lock_at() {     # yes / no: the commit carries gpsp/tools/layout/layout.lock
    local c="$1" k
    for k in "${LOCK_AT[@]+"${LOCK_AT[@]}"}"; do [ "${k%%=*}" = "$c" ] && { echo "${k#*=}"; return; }; done
    if G "$REPO" cat-file -e "$c:gpsp/tools/layout/layout.lock" >/dev/null 2>&1; then k=yes
    elif G "$REPO" cat-file -e "$c^{commit}" >/dev/null 2>&1; then k=no
    else k=yes; fi      # a commit this repo does not have: held to the strict rule
    LOCK_AT+=("$c=$k"); echo "$k"
  }
  benv_cmp() { json_get "$1" buildEnv | tr ' ' '\n' | grep -vE '^GPSP_BUILD_(COMMIT|TREE)=' | tr '\n' ' '; }
  jbool() { sed -nE "s/.*\"$2\": *(true|false).*/\1/p" "$1" | head -1; }
  REF_COMMIT=$(json_get "$AGAINST/release/flavour.json" sourceCommit)
  REF_GTREE=$(json_get "$AGAINST/release/flavour.json" gpspTree)
  for FL in "${RESULTS[@]}"; do
    RJ="$AGAINST/$FL/flavour.json"; NJ="$OUT/$FL/flavour.json"
    for j in "$RJ" "$NJ"; do
      [ "$(json_get "$j" hermetic)" = yes ] || die 2 "--against [$FL]: $j is not a hermetic build (no buildEnv record: built before 2026-11-07's flavours.sh?): rebuild it"
      [ "$(jbool "$j" workingTreeClean)" = true ] || die 2 "--against [$FL]: $j has workingTreeClean false: a dirty tree is not a gate input"
      [ "$(json_get "$j" sourceCommit)" = "$(json_get "$(dirname "$j")/../release/flavour.json" sourceCommit)" ] ||
        die 2 "--against [$FL]: $j names another sourceCommit than its run's release flavour"
    done
    for k in dockerId buildVersion sourceDateEpoch rfuSched; do
      a=$(json_get "$RJ" "$k"); b=$(json_get "$NJ" "$k")
      [ -n "$a" ] && [ -n "$b" ] || die 2 "--against [$FL]: '$k' missing in $([ -n "$a" ] && echo "$NJ" || echo "$RJ") (a build from before the pins? rebuild it)"
      [ "$a" = "$b" ] || die 2 "--against [$FL]: $k differs: reference '$a', this build '$b' (pin it: DOCKER_IMAGE=...@sha256, --build-version, --sde)"
    done
    a=$(json_get "$RJ" extraEnv); b=$(json_get "$NJ" extraEnv)
    [ "$a" = "$b" ] || die 2 "--against [$FL]: extraEnv differs: reference '$a', this build '$b'"
    a=$(benv_cmp "$RJ"); b=$(benv_cmp "$NJ")
    [ "$a" = "$b" ] || die 2 "--against [$FL]: buildEnv differs: reference '$a', this build '$b'"
    case "$FL" in
      release|lab)
        for j in "$RJ" "$NJ"; do
          if [ "$(lock_at "$(json_get "$j" sourceCommit)")" = yes ]; then
            [ "$(json_get "$j" layoutLock)" = "$FL:ok" ] || die 2 "--against [$FL]: $j layoutLock is '$(json_get "$j" layoutLock)', not $FL:ok"
          else
            # a commit from before tools/layout/layout.lock (01067e74): nothing to hold,
            # and nothing may claim a result (the LL verdict says which)
            [ -z "$(json_get "$j" layoutLock)" ] || [ "$(json_get "$j" layoutLock)" = none ] ||
              die 2 "--against [$FL]: $j records layoutLock '$(json_get "$j" layoutLock)' but its commit has no tools/layout/layout.lock"
          fi
        done ;;
    esac
  done
  NEW_COMMIT=$(json_get "$OUT/release/flavour.json" sourceCommit)
  NEW_PARENT=$(json_get "$OUT/release/flavour.json" sourceParent)
  NEW_GTREE=$(json_get "$OUT/release/flavour.json" gpspTree)
  if [ "$REF_COMMIT" = "$NEW_COMMIT" ]; then ANCESTRY="same commit ${NEW_COMMIT:0:10}"
  elif [ "$REF_COMMIT" = "$NEW_PARENT" ]; then ANCESTRY="reference = first parent ${REF_COMMIT:0:10}"
  elif [ -n "$REF_GTREE" ] && [ "$REF_GTREE" != unknown ] && [ "$REF_GTREE" = "$NEW_GTREE" ]; then ANCESTRY="same gpsp/ tree ${NEW_GTREE:0:10}"
  elif [ -n "$REF_ANCESTOR" ]; then
    # F4 (P-review 2026-11-08; PLAN-P-beta5 P7): an EXPLICITLY named ancestor (a merge's other
    # parent, e.g. the beta tip, or the P0c commit) is accepted, nothing implicit: the name must
    # resolve to the reference's own commit, and that commit must be an ancestor of this one
    RA=$(G "$REPO" rev-parse --verify -q "$REF_ANCESTOR^{commit}") || die 2 "--ref-ancestor: no such commit '$REF_ANCESTOR'"
    [ "$RA" = "$REF_COMMIT" ] || die 2 "--ref-ancestor ${RA:0:10} is not the reference's commit ${REF_COMMIT:0:10} (name the commit the reference was built from)"
    MB=$(G "$REPO" merge-base "$REF_COMMIT" "$NEW_COMMIT" 2>/dev/null) || MB=""
    [ "$MB" = "$REF_COMMIT" ] || die 2 "--ref-ancestor: the reference ${REF_COMMIT:0:10} is not an ancestor of this build's ${NEW_COMMIT:0:10} (merge-base ${MB:-none})"
    PARENTS=$(G "$REPO" rev-list --parents -n 1 "$NEW_COMMIT") || die 2 "--ref-ancestor: cannot list the parents of ${NEW_COMMIT:0:10}"
    k=0; PIDX=""
    for p in $PARENTS; do [ "$p" = "$REF_COMMIT" ] && PIDX=$k; k=$((k + 1)); done
    if [ -n "$PIDX" ]; then ANCESTRY="reference = named ancestor ${REF_COMMIT:0:10}, parent $PIDX of ${NEW_COMMIT:0:10} (--ref-ancestor)"
    else ANCESTRY="reference = named ancestor ${REF_COMMIT:0:10}, $(G "$REPO" rev-list --count "$REF_COMMIT..$NEW_COMMIT") commit(s) before ${NEW_COMMIT:0:10} (--ref-ancestor)"; fi
  else die 2 "--against: the reference ${REF_COMMIT:0:10} is not this build's commit, its first parent (${NEW_PARENT:0:10}) or a commit with its gpsp/ tree: nothing ties the step to its parent (an ancestor is accepted only when named: --ref-ancestor REV)"
  fi
  note "--against contract ok ($ANCESTRY; env, lock, clean tree, three flavours)"
  : > "$OUT/checks.txt"
  echo "-- contract: $ANCESTRY; buildEnv '$(benv_cmp "$OUT/release/flavour.json")'" >> "$OUT/checks.txt"
  # the identity gates, from the gate line's own list (none if that tool is absent: then
  # every difference is a FAIL)
  IDENTITY_GATES=$("$PY" "$HERE/flavours_gate.py" --identity 2>/dev/null)
  chk() {   # chk <flavour|-> <gate> <label> <cmd...>
    local fl="$1" gate="$2" label="$3" rc v t; shift 3
    echo "-- $label: $*" >> "$OUT/checks.txt"
    t=$(mktemp "${TMPDIR:-/tmp}/flavours-chk-out.XXXXXX")
    "$@" > "$t" 2>&1; rc=$?
    cat "$t" >> "$OUT/checks.txt"
    if [ $rc = 0 ]; then v=PASS
    elif [ $rc = 1 ] && [[ " $IDENTITY_GATES " == *" $gate "* ]] &&
         ! grep -qE 'Traceback \(most recent call last\)|unbound variable|command not found|syntax error' "$t"; then
      v=DIFF   # the tool ran and found a difference; whether that fails is the gate line's call
    else v=FAIL; label="$label; rc $rc"; fi
    rm -f "$t"
    note "check $label: $v"; echo "== $fl $gate $v ($label)" >> "$OUT/checks.txt"
    if [ $v = FAIL ] || { [ $v = DIFF ] && [ "$EXPECT_CHANGE" = 0 ]; }; then CHECK_RC=6; fi
  }
  failchk() {  # failchk <flavour|-> <gate> <why>: a check that could not run is a FAIL
    note "check $1 $2: FAIL ($3)"; echo "== $1 $2 FAIL ($3)" >> "$OUT/checks.txt"; CHECK_RC=6
  }
  need() { [ -f "$HERE/$1" ]; }
  stack_same() {   # ST: the verdict passes on NEW, and TOTAL/unmatched lines equal REF's
    local a b
    # exit 2 (FAIL even with --expect-code-change): the new build's stack verdict, or no
    # TOTAL line on either side; exit 1 (DIFF): both fine, the lines differ
    "$PY" "$HERE/stack_depth.py" "$N/gpsp_adhoc.elf" --su "$N/su" --resolve "$HERE/stack_resolve.txt" --tree 0 > "$N/stack_depth.txt" 2>&1 || { cat "$N/stack_depth.txt"; return 2; }
    "$PY" "$HERE/stack_depth.py" "$R/gpsp_adhoc.elf" --su "$R/su" --resolve "$HERE/stack_resolve.txt" --tree 0 > "$N/stack_depth.ref.txt" 2>&1
    a=$(grep -E "TOTAL|unmatched" "$N/stack_depth.ref.txt"); b=$(grep -E "TOTAL|unmatched" "$N/stack_depth.txt")
    echo "ref: $a"; echo "new: $b"; [ -n "$a" ] && [ -n "$b" ] || return 2; [ "$a" = "$b" ]
  }
  # ---- source checks: both sides' gpsp/ exported from their commits
  CHK=$(mktemp -d "${TMPDIR:-/tmp}/flavours-chk.XXXXXX")
  export_to() {   # export_to <commit> <dir>
    mkdir -p "$2" && GB "$REPO" archive --format=tar --prefix=gpsp/ "$1:gpsp" > "$2.tar" && tar -x -C "$2" -f "$2.tar" && rm -f "$2.tar"
  }
  SRC_OK=1
  export_to "$REF_COMMIT" "$CHK/ref" || { SRC_OK=0; failchk - G0 "git archive of the reference ${REF_COMMIT:0:10} failed"; }
  export_to "$NEW_COMMIT" "$CHK/new" || { SRC_OK=0; failchk - G0 "git archive of this build's ${NEW_COMMIT:0:10} failed"; }
  RS="$CHK/ref/gpsp"; NS="$CHK/new/gpsp"
  hotord_same() {  # HO: placement passes on NEW; the report equals REF's (staleness recorded, not gated alone)
    # each side's own psp/layout/hot_order.txt + hot.ord (the exported source), never the
    # copy next to these tools (a tools snapshot has none; another tree's may differ)
    # exit 2 (FAIL even with --expect-code-change): no source, or NEW's placement fails;
    # exit 1 (DIFF): the report differs from REF's (jsoneq)
    [ "$SRC_OK" = 1 ] || { echo "no source export"; return 2; }
    "$PY" "$HERE/hotord_check.py" --build "$N" --order "$NS/psp/layout/hot_order.txt" --ord "$NS/psp/layout/hot.ord" \
      --sizes warn --json "$N/hotord.json" || return 2
    "$PY" "$HERE/hotord_check.py" --build "$R" --order "$RS/psp/layout/hot_order.txt" --ord "$RS/psp/layout/hot.ord" \
      --sizes warn --json "$N/hotord.ref.json" > /dev/null 2>&1
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
          --flavour "$FL" --golden "$HERE/strong_syms.golden.tsv"
      else failchk "$FL" SS "no gpsp_adhoc.map (built with --no-sidecars)"; fi
    else failchk "$FL" SS "strong_syms.py absent"; fi
    if need stack_depth.py; then
      if [ -d "$N/su" ] && [ -d "$R/su" ]; then chk "$FL" ST "$FL stack_depth" stack_same
      else failchk "$FL" ST "no su/ (built with --no-sidecars)"; fi
    else failchk "$FL" ST "stack_depth.py absent"; fi
    # LL: the lock the contract above required, as a verdict of the gate line
    case "$FL" in release|lab)
      if [ "$(lock_at "$NEW_COMMIT")" = yes ] || [ "$(lock_at "$REF_COMMIT")" = yes ]; then
        echo "== $FL LL PASS (layoutLock ref $(json_get "$R/flavour.json" layoutLock), new $(json_get "$N/flavour.json" layoutLock))" >> "$OUT/checks.txt"
      else
        echo "== $FL LL PASS (no tools/layout/layout.lock at either commit: nothing to hold; BF holds the pads)" >> "$OUT/checks.txt"
      fi ;;
    esac
  done
  if [ "$SRC_OK" = 1 ]; then
    IMG=$(json_get "$OUT/release/flavour.json" dockerImage)
    PSDE=$(json_get "$OUT/release/flavour.json" sourceDateEpoch); [ "$PSDE" = none ] && PSDE=""
    for FL in "${RESULTS[@]}"; do
      if need pp_diff.sh; then chk "$FL" G0 "$FL pp_diff main_psp.c" env DOCKER_IMAGE="$IMG" bash "$HERE/pp_diff.sh" --ref "$RS" --new "$NS" --logs "$AGAINST" --flavours "$FL" --sde "$PSDE"
      else failchk "$FL" G0 "pp_diff.sh absent"; fi
    done
    PRUNES=(); for p in "${PRUNE_ALLOW[@]+"${PRUNE_ALLOW[@]}"}"; do PRUNES+=(--allow-prune "$p"); done
    if need pp_sweep.sh; then chk - G0var "pp_sweep every switch and pair, 4 flavours, every changed file${PRUNE_ALLOW[*]:+; allowed prunes: ${PRUNE_ALLOW[*]}}" \
        env DOCKER_IMAGE="$IMG" bash "$HERE/pp_sweep.sh" --ref "$RS" --new "$NS" --logs "$AGAINST" --sde "$PSDE" "${PRUNES[@]+"${PRUNES[@]}"}"
    else failchk - G0var "pp_sweep.sh absent"; fi
    cf_same() {   # CF: check_flags rc 0 on both trees, identical output
      local s rc=0
      for s in ref new; do
        cp "$HERE/../check_flags.py" "$CHK/$s/gpsp/tools/check_flags.py" || return 1
        ( cd "$CHK/$s/gpsp" && "$PY" tools/check_flags.py ) > "$CHK/cf_$s.txt" 2>&1 || { echo "[$s] check_flags rc != 0"; rc=1; }
      done
      cat "$CHK/cf_new.txt"
      diff "$CHK/cf_ref.txt" "$CHK/cf_new.txt" || { echo "check_flags output differs from the reference's"; rc=1; }
      return $rc
    }
    if [ -f "$HERE/../check_flags.py" ]; then chk - CF "check_flags rc 0, output = reference's" cf_same
    else failchk - CF "tools/check_flags.py absent"; fi
    SCOPES=(); for g in "${SCOPE_ALLOW[@]+"${SCOPE_ALLOW[@]}"}"; do SCOPES+=(--scope-allow "$g"); done
    if need step_scope.sh; then
      chk - BF "layout.lock lab-pads.env psp/Makefile psp/layout tools/build.sh identical" bash "$HERE/step_scope.sh" bf --ref "$RS" --new "$NS"
      chk - DS "diff within psp/main_psp.c, psp/fe/${SCOPE_ALLOW[*]:+ + ${SCOPE_ALLOW[*]}}" bash "$HERE/step_scope.sh" ds --ref "$RS" --new "$NS" "${SCOPES[@]+"${SCOPES[@]}"}"
    else failchk - BF "step_scope.sh absent"; failchk - DS "step_scope.sh absent"; fi
    # FP: frame parity (owner decision 2026-11-17): the code that runs every frame is the same
    # in this build's release and lab flavours, up to the differences the recorded baseline
    # already had (tools/layout/frame_parity.py; the spec and baseline are the commit's own,
    # this tool tree's before a commit carried them)
    FPT="$HERE/../layout/frame_parity.py"
    FPS="$NS/tools/layout/frame_parity.spec"; [ -f "$FPS" ] || FPS="$HERE/../layout/frame_parity.spec"
    FPB="$NS/tools/layout/frame_parity.base"; [ -f "$FPB" ] || FPB="$HERE/../layout/frame_parity.base"
    if [ ! -f "$FPT" ]; then failchk - FP "frame_parity.py absent"
    elif [ ! -f "$FPS" ] || [ ! -f "$FPB" ]; then failchk - FP "no frame_parity.spec/.base"
    elif [ ! -f "$OUT/release/gpsp_adhoc.elf" ] || [ ! -f "$OUT/lab/gpsp_adhoc.elf" ]; then failchk - FP "no release or lab ELF"
    else chk - FP "frame_parity release vs lab" "$PY" "$FPT" check "$OUT/release/gpsp_adhoc.elf" "$OUT/lab/gpsp_adhoc.elf" \
        --spec "$FPS" --base "$FPB" -v
    fi
  fi
  # CI: the build-time run on the exact source; --check-only re-runs it on the export of
  # the same commit with THESE tools (a record from older tools is not a verdict of these)
  if [ -z "$CI_RC" ]; then
    if [ "$SRC_OK" = 1 ] && [ -f "$HERE/ci_greps.py" ]; then
      "$PY" "$HERE/ci_greps.py" --src "$NS" --phase "$PHASE" > "$OUT/ci_greps.txt" 2>&1; CI_RC=$?
    elif [ "$SRC_OK" = 1 ]; then CI_RC="absent (no ci_greps.py)"; else CI_RC="no source export"; fi
  fi
  { echo "-- ci_greps --phase $PHASE (build-time, $OUT/ci_greps.txt):"; cat "$OUT/ci_greps.txt" 2>/dev/null; } >> "$OUT/checks.txt"
  case "$CI_RC" in
    0) note "check ci_greps: PASS"; echo "== - CI PASS (ci_greps --phase $PHASE)" >> "$OUT/checks.txt" ;;
    *) failchk - CI "ci_greps rc ${CI_RC:-not run}" ;;
  esac
  rm -rf "$CHK"
  # the gate line, generated from checks.txt
  GATE_ARGS=(); [ "$EXPECT_CHANGE" = 1 ] && GATE_ARGS=(--expect-code-change)
  "$PY" "$HERE/flavours_gate.py" "${GATE_ARGS[@]+"${GATE_ARGS[@]}"}" "$OUT/checks.txt" > "$OUT/gate.txt" || CHECK_RC=6
  note "gate: $(cat "$OUT/gate.txt")"
fi

[ "$CHECK_ONLY" = 1 ] || {
  echo '{'
  echo "  \"sourceCommit\": \"$REV\", \"sourceTree\": \"$TREE\", \"workingTreeClean\": ${CLEAN:-false},"
  echo "  \"sourceDateEpoch\": \"$SDE\", \"rfuSched\": \"$RFU_SCHED\", \"buildVersion\": \"${BUILD_VERSION:-}\","
  echo "  \"dockerImage\": \"$DOCKER_IMAGE\", \"dockerId\": \"${IMG_ID:-}\", \"ciGrepsRc\": \"${CI_RC:-}\", \"phase\": \"$PHASE\","
  echo "  \"expectCodeChange\": $([ "$EXPECT_CHANGE" = 1 ] && echo true || echo false),"
  echo "  \"flavours\": \"${RESULTS[*]}\", \"against\": \"$AGAINST\", \"checksRc\": $CHECK_RC"
  echo '}'
} > "$OUT/flavours.json"
note "done: ${RESULTS[*]}  (checks rc $CHECK_RC)"
exit $CHECK_RC
