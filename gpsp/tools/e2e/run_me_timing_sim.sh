#!/usr/bin/env bash
# run_me_timing_sim.sh -- the desktop oracle for the Media Engine renderer's
# snapshot TIMING (video_me_timing_sim.h, docs/ME-RENDERER-DIVERGENCE.md).
#
# PPSSPP never runs the PSP's second core, so the ME cannot be executed off
# hardware.  What CAN be reproduced is its input: the same capture and the
# same vram/oam/palette the ME would copy at a given post point, replayed
# through the same video.cc.  This builds the SDL twin with -DME_TIMING_SIM=1,
# runs a hardware A/B fixture with the hardware A/B's own input script, and
# reports, for the post-at-frame-end ("post") and post-at-vcount-160 ("vis")
# engines, how many frames match no CPU-rendered frame within +-8.
#
# Run it under WSL (native gcc + SDL2), from anywhere:
#   tools/e2e/run_me_timing_sim.sh heart_soul_heavy [BATTLE_INPUTS]
#
# Environment:
#   MTS_WORK  build/run directory on the Linux filesystem (default ~/me-timing-sim;
#             /mnt/c is far too slow for the per-frame logs and dumps)
#   MTS_FIX   fixture directory (default builds/unbound-soak/fixtures of the
#             main checkout: <fixture>.st0, the .sav files, manifest.json)
#   MTS_ROMS  directory holding the ROMs and gba_bios.bin
#             (default builds/unbound-harness/fixtures of the main checkout)
#   MTS_DUMP=1  also write raw cpu/post/vis images of every divergent frame
#
# The desktop link needs two stand-ins that a PSP build never does: the GB core
# (the SDL Makefile does not archive gbcore/) and smc_gates_refresh_values
# (savestate.c calls it under HAVE_DYNAREC; cpu_threaded.c defines it only with
# SMC_GATES).  Both are generated as no-op stubs from the link errors; the GBA
# path never reaches them.
set -euo pipefail
FIXTURE=${1:?usage: run_me_timing_sim.sh FIXTURE [BATTLE_INPUTS]}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"
# The fixtures live in the main checkout; a worktree under builds/ finds it by
# walking up (WSL cannot follow a worktree's Windows-path .git file).
MAIN=$SRC
while [ ! -d "$MAIN/builds/unbound-soak" ] && [ "$MAIN" != / ]; do MAIN=$(dirname "$MAIN"); done
WORK=${MTS_WORK:-$HOME/me-timing-sim}
FIX=${MTS_FIX:-$MAIN/builds/unbound-soak/fixtures}
ROMS=${MTS_ROMS:-$MAIN/builds/unbound-harness/fixtures}
INPUTS=${2:-$MAIN/builds/opus-perf-harness/campaign-psp3000-split/jobs/rend_r01_heart_soul_heavy_me1/battle.inputs}
case $FIXTURE in heart_soul*) ROM="Pokemon Heart & Soul";; unbound*) ROM="Pokemon Unbound";;
  *) echo "unknown fixture family: $FIXTURE" >&2; exit 2;; esac

# ---- build ----------------------------------------------------------------
B=$WORK/src; mkdir -p "$B"
rsync -a --delete --exclude .git --exclude builds --exclude releases --exclude docs \
  --exclude '*.gba' --exclude '*.sav' --exclude 'sdl/obj' --exclude 'sdl/gpsp_sdl' \
  --exclude 'sdl/*.a' --exclude '**/*.o' --exclude 'psp/me/*.prx' --exclude 'psp/me/*.elf' \
  "$SRC/" "$B/"
cd "$B"
make platform=unix clean >/dev/null 2>&1 || true
make platform=unix -j"$(nproc)" >"$WORK/core.log" 2>&1 || true   # the .so link may fail; the objects are what we need
g++ -I./libretro -I./libretro/libretro-common/include -I./ \
  -DMMAP_JIT_CACHE -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H \
  -D__LIBRETRO__ -DINLINE=inline -Wall -DHAVE_DYNAREC -DX86_ARCH -fPIC \
  -DFRONTEND_SUPPORTS_RGB565 -fno-rtti -fno-exceptions -std=c++11 \
  -O3 -DNDEBUG -DME_TIMING_SIM=1 -c -o video.o video.cc
archive() {
  rm -f sdl/libgpsp_core.a
  ar rcs sdl/libgpsp_core.a $(find . -name '*.o' -not -path './sdl/*' -not -path './psp/*' \
    -not -path './frontend-common/*' -not -path './netdrv/*' -not -path './tools/*' \
    -not -path './tests/*' -not -path './mips/*' -not -path './gbcore/*' \
    -not -path './arm/*' -not -path './3ds/*' -not -path './jni/*')
}
archive
(cd sdl && rm -f gpsp_sdl obj/main_sdl.o && make gpsp_sdl >"$WORK/link.log" 2>&1) || {
  grep -o "undefined reference to \`[A-Za-z_0-9]*" "$WORK/link.log" | sed 's/.*`//' | sort -u |
    awk '{print "long " $1 "() { return 0; }"}' > mts_stubs.c
  [ -s mts_stubs.c ] || { tail -20 "$WORK/link.log"; exit 1; }
  cc -w -O2 -c -o mts_stubs.o mts_stubs.c
  archive
  (cd sdl && make gpsp_sdl >"$WORK/link.log" 2>&1) || { tail -20 "$WORK/link.log"; exit 1; }
}

# ---- run ------------------------------------------------------------------
O=$WORK/runs/$FIXTURE; rm -rf "$O"; mkdir -p "$O/dump"
cp "$FIX/$ROM.sav" "$O/game.sav"; cp "$FIX/$FIXTURE.st0" "$O/state.st0"
[ "${MTS_DUMP:-0}" = 1 ] && export MTS_DUMP_DIR="$O/dump"
(cd sdl && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 1800 ./gpsp_sdl \
  --rom "$ROMS/$ROM.gba" --bios-dir "$ROMS" --save "$O/game.sav" --state "$O/state.st0" \
  --script "$INPUTS" --me-capture-test --ff --no-pacing --autoexit 20000 \
  --log "$O/frontend.log" --dump-dir "$O" 2>"$O/mts.log")
grep -E "state_load|ap_done|ap_fail" "$O/frontend.log" || true
python3 "$SRC/tools/me_timing_sim_report.py" "$O/mts.log" 300 3900
