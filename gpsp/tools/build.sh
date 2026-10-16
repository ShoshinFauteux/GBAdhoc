#!/usr/bin/env bash
# build.sh — THE build command.  One profile name, one artifact, one manifest.
#
#   tools/build.sh release      what a player installs
#   tools/build.sh harness      the hardware performance rig (telemetry+autopilot)
#   tools/build.sh harness64    the same rig with the 64 MiB layout (MEMSIZE=1)
#   tools/build.sh diagnostic   release plus investigation instruments
#   tools/build.sh soak         the rig plus the bad-jump reporter, for long runs
#
# Options:
#   --no-clean     skip the clean rebuild.  ONLY safe when no define changed;
#                  neither makefile tracks CFLAGS (see below).
#   --out DIR      where to put the manifest and a copy of the artifacts
#   --expect TOK   a string that must be present in the linked ELF; repeatable
#
# WHY A SINGLE SCRIPT.  The three flavours were defined in four different
# places -- ./build.sh, tools/build_perf.py, tools/build_stability.sh and
# releases/*/build.sh -- each with its own copy of a long flag list.  That is
# how 7283f13 shipped with BADJUMP_REPORT's Memory Stick logging: the release
# recipe and the diagnostic recipe were separate texts that drifted.  The flag
# lists now live HERE and nowhere else, and gpsp_profile.h refuses the
# combinations that are silently wrong.
#
# WHAT THIS SCRIPT PROTECTS AGAINST, each learned the hard way:
#
#  * Neither makefile tracks define changes, so a build after a flag change with
#    stale .o files silently drops the flag and the experiment reads as "no
#    effect".  Default is therefore a full clean, psp/*.o included.
#  * psp-fixup-imports fails as a WARNING and make still exits 0, leaving an
#    EBOOT whose syscall imports are broken -- it builds clean and dies on
#    hardware.  Grepped for explicitly.
#  * A container that failed could be papered over by the previous build's
#    artifacts, so the EBOOT must end up newer than every source.
#  * Reading the linked binary is the only check that actually answers for
#    symbols AND string literals; psp-nm missed both a static function and a
#    changed format string.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 9
ROOT="$PWD"
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

# WSL cannot read the Windows absolute path stored in a linked-worktree .git
# file. Allow the caller to supply the Windows Git metadata explicitly; a
# normal Linux checkout still discovers everything here as before.
BUILD_COMMIT="${GPSP_BUILD_COMMIT:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}"
BUILD_TREE="${GPSP_BUILD_TREE:-$(git rev-parse 'HEAD^{tree}' 2>/dev/null || echo unknown)}"
if [ "${GPSP_BUILD_DIRTY+x}" = x ]; then
  BUILD_DIRTY="$GPSP_BUILD_DIRTY"
else
  BUILD_DIRTY="$(git status --porcelain 2>/dev/null | head -1)" || BUILD_DIRTY=unknown
fi
BUILD_VERSION="${GPSP_BUILD_VERSION:-${BUILD_COMMIT:0:7}}"

DOCKER_IMAGE="pspdev/pspdev"

# ------------------------------------------------------------- profiles ----
# Core flags reach the ROOT make (anything touching the dynarec); frontend
# defines reach psp/Makefile.  Keep them together so a profile is one thing.
#
# SHARED CORE FLAGS: the dynarec configuration the accepted candidate was
# validated with.  Changing these changes emulation, so they are not per
# profile -- a diagnostic build must be the same emulator as the release or it
# answers a different question.
# JIT_CODE_DISCIPLINE (2026-10-02, 3.1.0 beta): survived every 40-reload soak on
# the Go and the 3000, corpus 26/26 identical; it never hands the kernel an
# I-cache range >= 16 KiB (proven a silent no-op on hardware).
CORE_COMMON="SMC_GATES=1 SMC_GATES_SIMPLE=1 SMC_GATES_RANKED=1 SMC_GATE_BITMAP=1
SMC_PARTIAL_SAFE=1 SMC_PARTIAL_STABLE_THUNK=1 SMC_PARTIAL_DIRECT_LINKS=1
GBA_PC_MASK=1 BADJUMP_SAFE=1 SMC_GATE_CHARGE=1 DISPATCH_CYCLE_CHECK=1 DMA_SMC_FLUSH=1
DISPATCH_CACHE=1 SMC_RETIRE_WINDOW=1 SERIAL_IDLE_FAST=1
JIT_CODE_DISCIPLINE=1 ENTRY_GATES=1
SCHED_FAST=1 IRQ_IO_FAST=1"
# SCHED_FAST / IRQ_IO_FAST (2026-10-11, 4.0.0 beta; docs/GBA-LINK-MIRROR.md
# 24-25): two exact scheduler / interrupt-register fast paths, for every game.
# The build switch compiles them in; the run-time switches sched_fast and
# irq_io_fast turn them on (psp/main_psp.c PLAY_SCHED_FAST / PLAY_IRQ_IO_FAST,
# config.ini keys of the same names as kill switches).  Corpus oracle:
# state-identical to the build without them over the whole library
# (DRCORPUS_SHIP=1 tools/drprof/dr_corpus.py full base fast2 fast2off).
# NOT here, on purpose: TIMER_LAZY (refuted: guest-visible, no gain),
# DRPROF_HW (profiler build), HOT_ORD=layout/hot_pair.ord (withdrawn).

profile_flags() {   # sets CORE_FLAGS, FE_DEFS, TITLE, KIND
  PSP_MAKE_FLAGS=""
  case "$1" in
    release)
      CORE_FLAGS="$CORE_COMMON GPSP_PROFILE=release"
      FE_DEFS="-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_PROFILE_RELEASE=1"
      TITLE="GBAdhoc"
      # ME_CATCH=1 ships the Media Engine module WITH the CPU-exception screen
      # (psp/me/catcher.c).  2026-09-26: this exact PRX (164633a0) ran hours of
      # heavy Heart & Soul play on all three consoles without a single freeze,
      # while the stock PRX froze the PSP Go within ~2 h -- why is still open
      # (docs/BUILD-SWITCHES.md, ME_CATCH).  It also turns any hard crash into
      # a photographable register screen instead of a silent hang.
      #
      # PSP_EBOOT_ICON1: the animated XMB icon (tools/logo/make_icon1.py),
      # the release only -- rig EBOOTs keep their still icons, so a console
      # running a harness is never mistaken for a player's install.
      PSP_MAKE_FLAGS="ME_CATCH=1 PSP_EBOOT_ICON1=assets/gbadhoc_icon1.pmf"
      ;;
    harness)
      # Telemetry plus autopilot, so a job file can drive a console unattended.
      CORE_FLAGS="$CORE_COMMON GPSP_PROFILE=harness"
      FE_DEFS="-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_KEEP_TELEMETRY -DGPSP_PERF_RIG
-DGPSP_PROFILE_HARNESS=1"
      # The title is the only thing a tester sees on the XMB, so the harness
      # says so: a harness EBOOT must never be mistaken for a playable one.
      TITLE="GBAdhoc HARNESS"
      PSP_MAKE_FLAGS="PSP_LARGE_MEMORY=0"
      ;;
    harness64)
      # The harness with the 64 MiB layout (MEMSIZE=1), which is what a
      # 2000/3000/Go runs a PLAYABLE build with.  `harness` pins the 1000's
      # 32 MiB layout, so on a 3000 it can never hold a 32 MiB cart: the ROM
      # residency A/B (docs/ROM-RESIDENCY.md) needs this one.  Same core, same
      # frontend defines; only PARAM.SFO's MEMSIZE differs.
      CORE_FLAGS="$CORE_COMMON GPSP_PROFILE=harness"
      FE_DEFS="-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_KEEP_TELEMETRY -DGPSP_PERF_RIG
-DGPSP_PROFILE_HARNESS=1"
      TITLE="GBAdhoc HARNESS64"
      # ME_CATCH=1 as in the release: the residency rig runs on the PSP Go,
      # the console that hard-freezes, and a CPU exception there should leave
      # a photographable register screen rather than a silent hang.
      PSP_MAKE_FLAGS="PSP_LARGE_MEMORY=1 ME_CATCH=1"
      ;;
    soak)
      # The harness, plus the bad-jump reporter.  For long unattended runs whose
      # POINT is the failure: a freeze leaves no summary and no score, so the
      # only evidence is what reached the Memory Stick before it stopped.
      #
      # This is a harness build by every other measure -- same telemetry, same
      # autopilot, same GPSP_PROFILE_HARNESS for gpsp_profile.h -- so the guard
      # treats it as one.  BADJUMP_REPORT is legal here; it is forbidden only in
      # a release, which is the combination that actually shipped once.
      #
      # SMC_WRITE_HISTO is deliberately NOT set: it rewrites smchisto.txt every
      # 4000 flushes, and over ten minutes of fast-forward that is a lot of
      # Memory Stick traffic competing with the event log we actually want.
      CORE_FLAGS="$CORE_COMMON GPSP_PROFILE=soak BADJUMP_REPORT=1 XLAT_DEPTH_PROBE=1 IRQ_INTEGRITY_CHECK=1"
      FE_DEFS="-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_KEEP_TELEMETRY -DGPSP_PERF_RIG
-DGPSP_PROFILE_HARNESS=1"
      TITLE="GBAdhoc SOAK"
      PSP_MAKE_FLAGS="PSP_LARGE_MEMORY=0"
      ;;
    diagnostic)
      # Release, plus the instruments for the question currently being asked.
      # Both of these write to the Memory Stick; that is the point, and it is
      # exactly why gpsp_profile.h forbids them in a release.
      # 2026-09-24: the question is the EXECUTED bad jump (badjump_recover dumps
      # guest RAM at the fault).  SMC_WRITE_HISTO is left out: on Heart & Soul
      # it rewrites smchisto.txt every 4000 flushes, i.e. constant synchronous
      # Memory Stick writes on the emulation thread while the player is trying
      # to reproduce a timing-sensitive fault.
      # 2026-09-24 (later): plus IRQ_INTEGRITY_CHECK.  The H&S field fault had
      # r5, r6 and r8-r11 of the sprite loop at 08218fac replaced across one
      # call to a sprite-anim callee that saves only r4-r7 -- so r8-r11 changed
      # in something that ran INSIDE that call, and the prime suspect is an
      # interrupt handler (the VBlank/M4A mixer path) returning with the wrong
      # state.  PPSSPP never shows it (114k IRQ returns clean); the checker
      # asks the same question on hardware.  It compares
      # r0-r14/CPSR/return address at each IRQ return against the entry
      # snapshot (the BIOS and ABI guarantee equality) and dumps guest RAM to
      # ms0:/irqlost-N-*.bin on the first mismatches.  One struct copy per IRQ.
      CORE_FLAGS="$CORE_COMMON GPSP_PROFILE=diagnostic BADJUMP_REPORT=1 XLAT_DEPTH_PROBE=1 IRQ_INTEGRITY_CHECK=1"
      FE_DEFS="-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_PROFILE_DIAGNOSTIC=1"
      TITLE="GBAdhoc DIAG"
      ;;
    *)
      echo "unknown profile '$1'"
      echo "usage: tools/build.sh [release|harness|harness64|diagnostic|soak] [--no-clean] [--out DIR] [--expect TOK]"
      exit 2 ;;
  esac
  KIND="$1"
  CORE_FLAGS=$(printf '%s' "$CORE_FLAGS" | tr '\n' ' ')
  FE_DEFS=$(printf '%s' "$FE_DEFS" | tr '\n' ' ')
}

# ----------------------------------------------------------------- args ----
PROFILE=""; CLEAN=1; OUTDIR=""; EXPECT=()
while [ $# -gt 0 ]; do
  case "$1" in
    --no-clean) CLEAN=0; shift ;;
    --out)      OUTDIR="$2"; shift 2 ;;
    --expect)   EXPECT+=("$2"); shift 2 ;;
    -*)         echo "unknown option $1"; exit 2 ;;
    *)          PROFILE="$1"; shift ;;
  esac
done
[ -n "$PROFILE" ] || PROFILE=release
profile_flags "$PROFILE"
# GPSP_EBOOT_TITLE: the XMB title for a side-by-side test install (its own
# PSP/GAME folder next to the player's).  Only the title changes; the profile's
# flags do not, and the manifest records the title used.  relabel_pbp.py
# cannot do this for the release profile: "GBAdhoc" left no room in the SFO.
[ -n "${GPSP_EBOOT_TITLE:-}" ] && TITLE="$GPSP_EBOOT_TITLE"
# GBA_LINK=1: the GBA link mirror (two cores; docs/GBA-LINK-MIRROR.md).  A
# core flag AND a frontend flag, set here together so they cannot drift apart.
# A release may carry it (2026-10-03, GBA-LINK-MIRROR.md 17.6): instance B is
# dormant until a cable session starts, single-player output is unchanged, and
# the pinned layout keeps instance A exactly where a non-link build has it.
# Its cost is image size (instance B's static state), which the boot memory
# gate and docs/MEMORY-BUDGET.md account for.
if [ "${GBA_LINK:-0}" = 1 ]; then
  CORE_FLAGS="$CORE_FLAGS GBA_LINK=1"
  PSP_MAKE_FLAGS="$PSP_MAKE_FLAGS GBA_LINK=1"
  # GBALINK_LEAN_OUTLINE=1 (serial.h): the lean cable hooks out of line, so
  # update_gba keeps its single-player size in the pinned hot core.
  if [ "${GBALINK_LEAN_OUTLINE:-0}" = 1 ]; then
    CORE_FLAGS="$CORE_FLAGS GBALINK_LEAN_OUTLINE=1"
  fi
  [ -n "${GPSP_EBOOT_TITLE:-}" ] || [ "$KIND" = release ] || TITLE="$TITLE LINK"
fi

# One-off VARIANTS of a non-release profile (tools/drprof/stage_hw_drprof.py):
# REWIND=1: the rewind (docs/REWIND.md) compiled in, for any profile.  It is
# a switch of its own, like GBA_LINK, because a release refuses CORE_EXTRA and
# PSP_EXTRA: `GBA_LINK=1 REWIND=1 tools/build.sh release` is the playable
# build with the Settings toggle (which stays off until the player turns it
# on).  Both makes need it: the core's store marks and the frontend.
if [ "${REWIND:-0}" = 1 ]; then
  CORE_FLAGS="$CORE_FLAGS REWIND=1"
  PSP_MAKE_FLAGS="$PSP_MAKE_FLAGS REWIND=1"
fi

# CORE_EXTRA is appended to the ROOT make's flags (DISPATCH_CACHE=1,
# DRPROF_HW=1, ...), FE_EXTRA to the frontend defines.  Refused for a release:
# the release flag list lives in profile_flags and nowhere else.
if [ -n "${CORE_EXTRA:-}${FE_EXTRA:-}" ]; then
  [ "$PROFILE" = release ] && { echo "FAIL: CORE_EXTRA/FE_EXTRA with the release profile"; exit 1; }
  CORE_FLAGS="$CORE_FLAGS ${CORE_EXTRA:-}"
  FE_DEFS="$FE_DEFS ${FE_EXTRA:-}"
fi

# PSP_EXTRA: extra psp/Makefile variables for a one-off variant, e.g. the
# layout sweep's LAYOUT_PAD_TEXT=N (docs/JIT-CODE-DISCIPLINE.md).  Same rule.
if [ -n "${PSP_EXTRA:-}" ]; then
  [ "$PROFILE" = release ] && { echo "FAIL: PSP_EXTRA with the release profile"; exit 1; }
  PSP_MAKE_FLAGS="$PSP_MAKE_FLAGS $PSP_EXTRA"
fi

die()  { echo "FAIL: $*"; exit 1; }
note() { printf '%s\n' "$*"; }

NEWEST_SRC=$(find "$ROOT" \( -name '*.c' -o -name '*.cc' -o -name '*.h' \
             -o -name '*.S' -o -name '*.s' -o -name 'Makefile' \) \
             -type f -print | xargs ls -t 2>/dev/null | head -1)

run() {   # run <label> <workdir> <cmd...>
  local label="$1" wd="$2"; shift 2
  local outp rc
  outp=$(docker run --rm -v "$ROOT":/build -w "$wd" "$DOCKER_IMAGE" sh -c "$*" 2>&1)
  rc=$?
  # Project-owned errors are worth seeing immediately; the full log is not.
  printf '%s\n' "$outp" | grep -E 'error:|Error [0-9]+' | head -20
  if [ "$rc" -ne 0 ]; then
    printf '%s\n' "$outp" | tail -25
    die "$label container exited $rc"
  fi
  printf '%s\n' "$outp" | grep -q "stubs out of order" && \
    die "$label linked with BROKEN IMPORTS: a library named in psp/Makefile LIBS is already linked by psp-gcc's spec or build.mak's tail."
  printf '%s\n' "$outp" > "$ROOT/.build-$label.log"
  return 0
}

# NO NEW WARNINGS FROM GBADHOC-OWNED CODE.
#
# The tree reached zero project warnings on 2026-09-17, after clearing 68: six
# real defects (a %x scanf into a u32, four %u/uint32_t mismatches, one snprintf
# the compiler could prove may truncate) and the rest values that exist only to
# be reported, which look dead once fe_evt() compiles out.  Zero is only worth
# reaching if it is held, so a build that adds one fails here.
#
# Toolchain and SDK warnings are somebody else's code, excluded by path.  A
# project warning that genuinely cannot be fixed goes in tools/warning-allow --
# one grep -F pattern per line, with the reason beside it -- so it stays visible
# and reviewable instead of silently tolerated.
warning_gate() {
  local log="$1" label="$2" found allow
  [ -f "$log" ] || return 0
  allow="$ROOT/tools/warning-allow"
  found=$(grep -E 'warning:' "$log" |
          grep -vE '/usr/local/pspdev|/usr/lib/gcc|psp/sdk/include')
  if [ -n "$found" ] && [ -f "$allow" ] &&
     grep -qvE '^[[:space:]]*(#|$)' "$allow"; then
    found=$(printf '%s\n' "$found" |
            grep -vFf <(grep -vE '^[[:space:]]*(#|$)' "$allow"))
  fi
  if [ -n "$found" ]; then
    echo "FAIL: $label produced warnings in GBAdhoc-owned code:"
    printf '%s\n' "$found" | sed 's/^/    /' | head -30
    echo "    Fix them, or add a pattern and its reason to tools/warning-allow."
    exit 1
  fi
}

note "=== $PROFILE build in $ROOT ==="
if [ "$CLEAN" -eq 1 ]; then
  run clean /build "set -e; make platform=psp1 clean >/dev/null 2>&1; make -C psp clean >/dev/null 2>&1; rm -f psp/*.o psp/*.d"
fi
# LAYOUT_PIN (docs/LAYOUT-PINNING.md): ON by default since 2026-10-02.  Both
# gates passed: the whole-library corpus oracle (25/25 identical, dr_corpus
# `allpin`) and the PSP-3000 bench (AW2 5.56 -> 4.39 ms/frame, H&S 8.77 ->
# 8.43, and a 2600-byte pad no longer moves the pinned build).  Every profile
# gets it, so rigs measure the layout that ships.  LAYOUT_PIN=0 is the kill
# switch: the pre-pinning layout, byte for byte.  BOTH makes must get the
# same value.  LAYOUT_PAD_BYTES=N adds the stability probe (the bench's pad arm).
LAYOUT_PIN="${LAYOUT_PIN:-1}"
LAYOUT_PAD_BYTES="${LAYOUT_PAD_BYTES:-0}"
case "$LAYOUT_PIN" in 0|1) ;; *) die "LAYOUT_PIN must be 0 or 1" ;; esac
note "layout  : LAYOUT_PIN=$LAYOUT_PIN LAYOUT_PAD_BYTES=$LAYOUT_PAD_BYTES"
# CHEATS: the per-game cheats menu (docs/CHEATS.md), default on in every
# profile.  CHEATS=0 is the kill switch, and it must reach BOTH makes (the
# core half is cheats.c, the frontend half fe_cheats.c + the menu).
CHEATS="${CHEATS:-1}"
case "$CHEATS" in 0|1) ;; *) die "CHEATS must be 0 or 1" ;; esac
note "cheats  : CHEATS=$CHEATS"
run core  /build      "make platform=psp1 GIT_VERSION='$BUILD_VERSION' $CORE_FLAGS LAYOUT_PIN=$LAYOUT_PIN CHEATS=$CHEATS -j4"
[ -f "$ROOT/gpsp_libretro_psp1.a" ] || die "core archive missing"
run eboot /build/psp  "make EXTRA_DEFS='$FE_DEFS' PSP_EBOOT_TITLE='$TITLE' $PSP_MAKE_FLAGS LAYOUT_PIN=$LAYOUT_PIN LAYOUT_PAD_BYTES=$LAYOUT_PAD_BYTES CHEATS=$CHEATS"
[ -f "$ROOT/psp/EBOOT.PBP" ] || die "EBOOT missing"

# HOT-CORE BUDGET (docs/GBA-LINK-MIRROR.md 24-25).  psp/layout/hot_order.txt was
# generated for the function sizes of one build; a hot function that grows
# moves everything linked behind it (update_gba +688 bytes cost the link pair
# 8,400 I-cache misses a frame on a PSP-3000 and nothing in the build said
# so).  Report the core against its budget on every pinned build;
# HOT_BUDGET_STRICT=1 makes an overrun fail the build.
if [ "$LAYOUT_PIN" = 1 ] && [ -f "$ROOT/psp/gpsp_adhoc.elf" ]; then
  docker run --rm -v "$ROOT":/build -w /build "$DOCKER_IMAGE" sh -c     "psp-nm -S -n psp/gpsp_adhoc.elf" > "$ROOT/.build-elf.nm" 2>/dev/null || true
  PYB=python3; command -v python3 >/dev/null 2>&1 || PYB=python
  # a native Windows python needs native paths (MSYS_NO_PATHCONV is set above)
  HB_PY="$ROOT/tools/layout/hot_budget.py"; HB_ORD="$ROOT/psp/layout/hot_order.txt"; HB_NM="$ROOT/.build-elf.nm"
  case " $PSP_MAKE_FLAGS " in *" HOT_ORD=layout/hot_pair.ord "*) HB_ORD="$ROOT/psp/layout/hot_pair_order.txt" ;; esac
  if command -v cygpath >/dev/null; then
    HB_PY=$(cygpath -w "$HB_PY"); HB_ORD=$(cygpath -w "$HB_ORD"); HB_NM=$(cygpath -w "$HB_NM")
  fi
  if [ "${HOT_BUDGET_STRICT:-0}" = 1 ]; then
    "$PYB" "$HB_PY" "$HB_ORD" "$HB_NM" --strict       || die "the pinned hot core overran its budget (tools/layout/regen.sh)"
  else
    "$PYB" "$HB_PY" "$HB_ORD" "$HB_NM" || true
  fi
fi

warning_gate "$ROOT/.build-core.log"  "core"
warning_gate "$ROOT/.build-eboot.log" "eboot"

if [ -n "$NEWEST_SRC" ] && [ "$NEWEST_SRC" -nt "$ROOT/psp/EBOOT.PBP" ]; then
  die "EBOOT.PBP is OLDER than $NEWEST_SRC -- the build did not run"
fi
NEWEST_PSP_ASSET=$(find "$ROOT/psp/assets" -type f -print 2>/dev/null |
                   xargs ls -t 2>/dev/null | head -1)
if [ -n "$NEWEST_PSP_ASSET" ] && [ "$NEWEST_PSP_ASSET" -nt "$ROOT/psp/EBOOT.PBP" ]; then
  die "EBOOT.PBP is OLDER than $NEWEST_PSP_ASSET -- rebuild the PSP artwork"
fi

# --------------------------------------------------------------- audit -----
# A release must not contain the strings that mean "this writes to the memory
# stick" or "this can be driven by a job file".  gpsp_profile.h already refuses
# the defines; this catches a path that reached them some other way.
ELF="$ROOT/psp/gpsp_adhoc.elf"
[ -f "$ELF" ] || ELF="$ROOT/psp/EBOOT.PBP"
ELF_NATIVE="$ELF"
command -v cygpath >/dev/null && ELF_NATIVE=$(cygpath -w "$ELF")

# WHAT "FORBIDDEN" MEANS HERE.  Only strings whose presence implies a LIVE
# write path.  It deliberately does NOT list fe_autopilot, CMD.TXT or
# log/frontend.log: those are present in every playable build by design
# (ADR-0067 keeps one code path and neutralises it by pointing the harness ini
# at a filename that cannot exist), so forbidding them would fail a correct
# build.  Which is why the EXPECT side below checks that neutralisation is
# actually compiled in, rather than trusting that it is.
FORBIDDEN=(); REQUIRE=()
case "$KIND" in
  release)
    FORBIDDEN=("badjump.txt" "smchisto.txt" "irqchk.txt"
               "home_stall_frame")   # Menu = HOME stall rig: GPSP_PERF_RIG only
    # ADR-0067: proof the harness ini was pointed at an impossible path, so a
    # leftover .gpsp-harness.ini on a player's card cannot drive their console.
    REQUIRE=(".playable-no-harness")
    ;;
  harness|harness64)
    REQUIRE=(".gpsp-harness.ini")
    ;;
  diagnostic)
    # The instruments are the point; assert they are really in.
    REQUIRE=("badjump.txt" "badjump-%u-iwram.bin" ".playable-no-harness" "irqchk.txt")
    FORBIDDEN=("smchisto.txt")
    ;;
  soak)
    # Soak is a live harness profile with the bad-jump reporter enabled.
    # It must retain the harness control channel, while keeping the noisy SMC
    # histogram disabled and the release-only harness neutralizer absent.
    REQUIRE=(".gpsp-harness.ini" "badjump.txt" "irqchk.txt")
    FORBIDDEN=("smchisto.txt" ".playable-no-harness")
    ;;
esac
EXPECT=("${EXPECT[@]+"${EXPECT[@]}"}" "${REQUIRE[@]+"${REQUIRE[@]}"}")

AUDIT=$(python3 - "$ELF_NATIVE" "${#FORBIDDEN[@]}" "${FORBIDDEN[@]+"${FORBIDDEN[@]}"}" \
        "${#EXPECT[@]}" "${EXPECT[@]+"${EXPECT[@]}"}" <<'PY'
import sys
path = sys.argv[1]
nf   = int(sys.argv[2]); i = 3
forb = sys.argv[i:i+nf]; i += nf
ne   = int(sys.argv[i]); i += 1
want = sys.argv[i:i+ne]
try:
    blob = open(path, 'rb').read()
except OSError as e:
    print("FAIL cannot read %s: %s" % (path, e)); sys.exit(1)
rc = 0
for t in forb:
    if t.encode() in blob:
        print("  FAIL FORBIDDEN in this build profile: %s" % t); rc = 1
    else:
        print("  ok   absent: %s" % t)
for t in want:
    if t.encode() in blob:
        print("  ok   present: %s" % t)
    else:
        print("  FAIL MISSING: %s" % t); rc = 1
sys.exit(rc)
PY
)
printf '%s\n' "$AUDIT"
printf '%s\n' "$AUDIT" | grep -q FAIL && die "binary audit"

# ------------------------------------------------------- memory budget -----
# docs/MEMORY-BUDGET.md.  The heap a console gives this binary at boot is
#     heap_plus_image(console) - image
# where image is the loaded ELF (first PT_LOAD .. end of .bss) and
# heap_plus_image is a property of the console, measured by tools/memprobe.
# A bigger image (the link build's second core: +5.4 MiB) comes straight out
# of the heap, so every boot-time reservation is checked here against the
# SMALLEST console, before anything reaches a card.  MEMSIZE does not help a
# PSP-1000, so the 1000 binds every profile; the Go is printed because it is
# the smallest 64 MiB console (5 MiB under PPSSPP's model).
#   BUDGET_HPI_1000  24,096,552  PPSSPP PSP-1000 (memprobe); hardware: see doc
#   BUDGET_HPI_GO    52,408,104  PPSSPP 64 MiB 57,650,984 - 5,242,880 (Go,
#                                 resrig 2026-09-27); hardware: see doc
#   BUDGET_BOOT_MIN   4,194,304  netdrv arena (1 MiB) + the GBA core's minimum
#                                 (MEM_GBA_CORE_MIN, 2 MiB + 64 KiB) + margin;
#                                 the link build's measured floor is 3.1-3.6 MB
BUDGET_HPI_1000=${BUDGET_HPI_1000:-24096552}
BUDGET_HPI_GO=${BUDGET_HPI_GO:-52408104}
BUDGET_BOOT_MIN=${BUDGET_BOOT_MIN:-4194304}
IMAGE_BYTES=$(python3 - "$ELF_NATIVE" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:4] != b'\x7fELF':
    print(0); sys.exit(0)
phoff, = struct.unpack_from('<I', d, 28)
phentsize, phnum = struct.unpack_from('<HH', d, 42)
lo, hi = None, 0
for i in range(phnum):
    t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + i * phentsize)
    if t == 1:  # PT_LOAD
        lo = va if lo is None else min(lo, va)
        hi = max(hi, va + msz)
print(hi - lo if lo is not None else 0)
PY
)
[ "${IMAGE_BYTES:-0}" -gt 0 ] || die "memory budget: cannot read the image size of $ELF"
BOOT_HEAP_1000=$((BUDGET_HPI_1000 - IMAGE_BYTES))
BOOT_HEAP_GO=$((BUDGET_HPI_GO - IMAGE_BYTES))
note "memory  : image $IMAGE_BYTES B; heap at boot PSP-1000 $BOOT_HEAP_1000 B, PSP Go $BOOT_HEAP_GO B (floor $BUDGET_BOOT_MIN)"
[ "$BOOT_HEAP_1000" -ge "$BUDGET_BOOT_MIN" ] || \
  die "memory budget: a PSP-1000 would boot this image with $BOOT_HEAP_1000 B of heap, under the $BUDGET_BOOT_MIN B floor (docs/MEMORY-BUDGET.md)"
[ "$BOOT_HEAP_GO" -ge "$BUDGET_BOOT_MIN" ] || \
  die "memory budget: a PSP Go would boot this image with $BOOT_HEAP_GO B of heap, under the $BUDGET_BOOT_MIN B floor (docs/MEMORY-BUDGET.md)"

# ------------------------------------------------------------ manifest -----
ELF_MD5=$(md5sum "$ROOT/psp/gpsp_adhoc.elf" 2>/dev/null | cut -d' ' -f1)
PBP_MD5=$(md5sum "$ROOT/psp/EBOOT.PBP"      | cut -d' ' -f1)
PBP_SHA=$(sha256sum "$ROOT/psp/EBOOT.PBP"   | cut -d' ' -f1)
PRX_SHA=$(sha256sum "$ROOT/psp/me/gbadhoc_me.prx" 2>/dev/null | cut -d' ' -f1)
HOME_PRX_SHA=$(sha256sum "$ROOT/psp/home/gbadhoc_home.prx" 2>/dev/null | cut -d' ' -f1)
[ -n "$HOME_PRX_SHA" ] || die "psp/home/gbadhoc_home.prx missing (Menu = HOME module)"
IMG_ID=$(docker image inspect --format '{{index .RepoDigests 0}}' "$DOCKER_IMAGE" 2>/dev/null \
         || docker image inspect --format '{{.Id}}' "$DOCKER_IMAGE" 2>/dev/null)
COMMIT="$BUILD_COMMIT"
TREE="$BUILD_TREE"

MANIFEST="$ROOT/psp/build-manifest.json"
{
  echo '{'
  echo "  \"profile\": \"$KIND\","
  echo "  \"title\": \"$TITLE\","
  echo "  \"builtAt\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
  echo "  \"sourceCommit\": \"$COMMIT\","
  echo "  \"sourceTree\": \"$TREE\","
  echo "  \"workingTreeClean\": $([ -z "$BUILD_DIRTY" -o "$BUILD_DIRTY" = 0 ] && echo true || echo false),"
  echo "  \"dockerImage\": \"$IMG_ID\","
  echo "  \"coreFlags\": \"$CORE_FLAGS\","
  echo "  \"frontendDefines\": \"$FE_DEFS\","
  echo "  \"pspMakeFlags\": \"$PSP_MAKE_FLAGS\","
  echo "  \"cleanRebuild\": $([ "$CLEAN" -eq 1 ] && echo true || echo false),"
  echo "  \"layoutPin\": $LAYOUT_PIN,"
  echo "  \"layoutPadBytes\": $LAYOUT_PAD_BYTES,"
  echo "  \"eboot\": { \"md5\": \"$PBP_MD5\", \"sha256\": \"$PBP_SHA\" },"
  echo "  \"elfMd5\": \"$ELF_MD5\","
  echo "  \"memory\": { \"imageBytes\": $IMAGE_BYTES, \"bootHeapPsp1000\": $BOOT_HEAP_1000, \"bootHeapPspGo\": $BOOT_HEAP_GO },"
  echo "  \"mePrxSha256\": \"$PRX_SHA\","
  echo "  \"homePrxSha256\": \"$HOME_PRX_SHA\""
  echo '}'
} > "$MANIFEST"

if [ -n "$OUTDIR" ]; then
  mkdir -p "$OUTDIR"
  cp "$MANIFEST" "$OUTDIR/" 2>/dev/null
  cp "$ROOT/psp/EBOOT.PBP" "$OUTDIR/" 2>/dev/null
  cp "$ROOT/psp/me/gbadhoc_me.prx" "$OUTDIR/" 2>/dev/null
  cp "$ROOT/psp/home/gbadhoc_home.prx" "$OUTDIR/" 2>/dev/null
  note "staged to $OUTDIR"
fi

note "profile : $KIND ($TITLE)"
note "core    : $CORE_FLAGS"
note "frontend: $FE_DEFS"
note "elf md5 : $ELF_MD5"
note "pbp md5 : $PBP_MD5"
note "manifest: $MANIFEST"
if [ -n "$BUILD_DIRTY" ] && [ "$BUILD_DIRTY" != 0 ]; then
  note "NOTE: working tree is dirty; this artifact is not reproducible from a commit."
fi
exit 0
