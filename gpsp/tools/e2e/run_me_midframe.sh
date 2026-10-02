#!/usr/bin/env bash
# run_me_midframe.sh -- CPU-vs-ME oracle for mid-frame state changes.
#
# Builds the SDL twin with -DME_TIMING_SIM=1 (video_me_timing_sim.h), which
# replays every captured frame exactly as psp/me/me_render_glue.cc does, with
# the graphics memory the ME would copy at the vcount-160 post, and compares
# it with the frame the CPU renderer drew.  Then runs the synthetic ROMs of
# tools/e2e/me_midframe/gen_roms.py (and, optionally, real ROMs) and reports,
# per ROM, how many frames the ME model draws differently from the CPU.
#
# Run it under WSL (native gcc + SDL2):
#   tools/e2e/run_me_midframe.sh                 # all synthetic cases
#   tools/e2e/run_me_midframe.sh aff_irq pal_dma # a subset
#   MMF_EXTRA_CFLAGS="-DME_AFFINE_LINES=0" tools/e2e/run_me_midframe.sh
#
# Environment:
#   MMF_WORK          build/run directory (default ~/me-midframe)
#   MMF_FRAMES        frames per ROM (default 240; the first 30 are skipped)
#   MMF_EXTRA_CFLAGS  extra -D flags for the whole core build (A/B arms)
#   MMF_NO_BUILD=1    reuse the last build
#   MMF_DUMP=1        write raw cpu/vis images of divergent frames
#
# The ROMs are assembled in a throwaway Debian container with
# binutils-arm-none-eabi (image `gba-as`, built on first use).
#
# The oracle is only an oracle if it can fail: the controls must read 0 and
# the unfixed build must read non-zero on every (a)-(d) case.  Both are
# checked in docs/ME-MIDFRAME.md.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"
WORK=${MMF_WORK:-$HOME/me-midframe}
FRAMES=${MMF_FRAMES:-240}
mkdir -p "$WORK/roms"

# ---- ROMs -----------------------------------------------------------------
ASM=$WORK/asm; rm -rf "$ASM"; mkdir -p "$ASM"
CASES=$(python3 "$SCRIPT_DIR/me_midframe/gen_roms.py" "$ASM")
if ! docker image inspect gba-as >/dev/null 2>&1; then
  printf 'FROM debian:stable-slim\nRUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq binutils-arm-none-eabi && rm -rf /var/lib/apt/lists/*\n' |
    docker build -q -t gba-as - >/dev/null
fi
docker run --rm -v "$ASM:/w" -w /w gba-as bash -c '
  set -e
  for s in *.s; do n=${s%.s}
    arm-none-eabi-as -mcpu=arm7tdmi -o $n.o $s
    arm-none-eabi-ld -Ttext=0x08000000 -e _start -o $n.elf $n.o
    arm-none-eabi-objcopy -O binary $n.elf $n.gba
  done'

# ---- build ----------------------------------------------------------------
B=$WORK/src
if [ "${MMF_NO_BUILD:-0}" != 1 ]; then
  mkdir -p "$B"
  rsync -a --delete --exclude .git --exclude builds --exclude releases --exclude docs \
    --exclude '*.gba' --exclude '*.sav' --exclude 'sdl/obj' --exclude 'sdl/gpsp_sdl' \
    --exclude 'sdl/*.a' --exclude '**/*.o' --exclude 'psp/me/*.prx' --exclude 'psp/me/*.elf' \
    "$SRC/" "$B/"
  cd "$B"
  make platform=unix clean >/dev/null 2>&1 || true
  make platform=unix -j"$(nproc)" EXTRA_CFLAGS="${MMF_EXTRA_CFLAGS:-}" \
    CFLAGS_EXTRA="${MMF_EXTRA_CFLAGS:-}" >"$WORK/core.log" 2>&1 || true
  # video.o again, with the model compiled in (and the A/B flags).
  g++ -I./libretro -I./libretro/libretro-common/include -I./ \
    -DMMAP_JIT_CACHE -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H \
    -D__LIBRETRO__ -DINLINE=inline -Wall -DHAVE_DYNAREC -DX86_ARCH -fPIC \
    -DFRONTEND_SUPPORTS_RGB565 -fno-rtti -fno-exceptions -std=c++11 \
    -O3 -DNDEBUG -DME_TIMING_SIM=1 ${MMF_EXTRA_CFLAGS:-} -c -o video.o video.cc
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
fi

# ---- run ------------------------------------------------------------------
# Two runs per ROM.  Mode 2 (--me-capture-test) captures AND renders, so the
# CPU image is the reference and the model's image is compared with it.  Mode
# 1 (--me-capture-mode1) is PRODUCTION: the core captures and skips the render
# (this is where ME_AFFINE_LINES keeps stepping the counters itself); there is
# no CPU image, so its model images are compared with the mode-2 CPU images of
# the same frames.  Emulation is deterministic, so they must agree.
run_one() {  # case mode outdir
  (cd "$B/sdl" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 300 ./gpsp_sdl \
    --rom "$ROMDIR/$1.gba" --bios-dir "$WORK/roms" --save "$3/game.sav" \
    --option gpsp_bios=builtin $2 --ff --no-pacing --autoexit "$FRAMES" \
    --log "$3/frontend.log" --dump-dir "$3" 2>"$3/mts.log") || true
}
ROMDIR=${MMF_ROMDIR:-$ASM}
SEL=${*:-$CASES}
printf '%-15s %6s %7s %8s %7s  %s\n' case frames vis_bad post_bad m1_bad verdict
for c in $SEL; do
  O=$WORK/runs/$c; rm -rf "$O"; mkdir -p "$O/dump" "$O/m1"
  if [ "${MMF_DUMP:-0}" = 1 ]; then export MTS_DUMP_DIR="$O/dump"; else unset MTS_DUMP_DIR; fi
  run_one "$c" --me-capture-test "$O"
  unset MTS_DUMP_DIR
  run_one "$c" --me-capture-mode1 "$O/m1"
  python3 - "$O/mts.log" "$O/m1/mts.log" "$c" <<'PYEOF'
import re, sys
pat = re.compile(r'^MTS f=(\d+) cpu=(\w+) post=(\w+) vis=(\w+)', re.M)
def rows(p):
    return {int(f): (c, po, v) for f, c, po, v in pat.findall(open(p, errors='replace').read())}
m2, m1, name = rows(sys.argv[1]), rows(sys.argv[2]), sys.argv[3]
fr = sorted(f for f in m2 if f > 30)
vis = sum(1 for f in fr if m2[f][2] != m2[f][0])
post = sum(1 for f in fr if m2[f][1] != m2[f][0])
m1bad = sum(1 for f in fr if f not in m1 or m1[f][2] != m2[f][0])
distinct = len(set(m2[f][0] for f in fr))
bad = vis or m1bad
verdict = 'NO FRAMES' if not fr else ('DIVERGES' if bad else 'match')
print('%-15s %6d %7d %8d %7d  %s (cpu images: %d distinct)' % (name, len(fr), vis, post, m1bad, verdict, distinct))
PYEOF
done
