#!/usr/bin/env bash
# sharp.sh -- the sharp-bilinear campaign (docs/SHARP-BILINEAR.md).
#
#   OFF  every existing filter x scale, GBA + GBC + GB, ambient on and off, on
#        the BASE build and on THIS build: the GE drawbuffer dumps must be
#        byte-identical (nothing changes while sharp is not selected).
#   CTRL one base config twice: the dumps are deterministic run to run, so an
#        identity above means something.
#   ON   sharp at every scale (1x and 2x must equal nearest), GB/GBC sizes,
#        ambient bars, all three fast-forward presets, and a screenshot
#        (dump_at) per arm.
#
# Needs DISPLAY_RIG_DIR with b-harness/ (this build) and base-harness/ (the
# parent commit), and the readme30 library beside it (see run.sh).  Every run
# pins its private Xvfb to XDISPLAY (default :118).  Compare with
# sharp_compare.py.
D=${DISPLAY_RIG_DIR:?set DISPLAY_RIG_DIR}
R=$D/../readme30/roms
export XDISPLAY=${XDISPLAY:-:118}
cd "$(dirname "$0")"
EM="ROMSRC=$R/emerald.gba ROMNAME=emerald.gba STATE=$R/states/emerald.st0 LOADST=1"
CR="ROMSRC=$R/crystal.gbc ROMNAME=crystal.gbc STATE=$R/states/crystal.gbc.st0 LOADST=1"
KB="ROMSRC=$R/kirby.gb ROMNAME=kirby.gb"
GE="FRAMES=230 GEEVERY=20"
KGE="FRAMES=720 GEEVERY=60"
HERO_EM="$D/../readme30/lib/hero/Pokemon - Emerald Version (USA, Europe).565"
HERO_CR="$D/../readme30/lib/hero/Pokemon - Crystal Version (USA, Europe) (Rev 1).565"

# run <build> <tag> VAR=val ...   (word-split on purpose: the VAR lists above)
run() { b=$1; tag=$2; shift 2; ( export "$@"; bash run.sh "$tag" "$D/$b-harness" setups/game.sh 600 >/dev/null ); echo "done $tag"; }
JOBS=${JOBS:-3}
throttle() { while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 1; done; }

off_arm() {   # off_arm <tag> <vars...>, on both builds
  tag=$1; shift
  throttle; run base "B-$tag" "$@" &
  throttle; run b    "N-$tag" "$@" &
}

if [ "${PHASE:-all}" = all ] || [ "$PHASE" = off ]; then
  # shellcheck disable=SC2086
  {
  off_arm g-s0-f0 $EM $GE CFG="scale_gba = 0\nfilter_gba = 0"
  off_arm g-s0-f1 $EM $GE CFG="scale_gba = 0\nfilter_gba = 1"
  off_arm g-s1-f0 $EM $GE CFG="scale_gba = 1\nfilter_gba = 0"
  off_arm g-s1-f1 $EM $GE CFG="scale_gba = 1\nfilter_gba = 1"
  off_arm g-s2-f0 $EM $GE CFG="scale_gba = 2\nfilter_gba = 0"
  off_arm g-s2-f1 $EM $GE CFG="scale_gba = 2\nfilter_gba = 1"
  off_arm g-s3-f1 $EM $GE CFG="scale_gba = 3\nfilter_gba = 1"
  off_arm g-s1-f1-amb $EM HERO="$HERO_EM" $GE CFG="scale_gba = 1\nfilter_gba = 1\nambient_gba = 1"
  off_arm g-s0-f0-amb $EM HERO="$HERO_EM" $GE CFG="scale_gba = 0\nfilter_gba = 0\nambient_gba = 1"
  off_arm c-s0-f0 $CR $GE CFG="console = 2\nscale_gbc = 0\nfilter_gbc = 0"
  off_arm c-s1-f1 $CR $GE CFG="console = 2\nscale_gbc = 1\nfilter_gbc = 1"
  off_arm c-s2-f0 $CR $GE CFG="console = 2\nscale_gbc = 2\nfilter_gbc = 0"
  off_arm k-s3-f1 $KB $KGE CFG="console = 1\nscale_gb = 3\nfilter_gb = 1"
  throttle; run base B2-g-s1-f1 $EM $GE CFG="scale_gba = 1\nfilter_gba = 1" &
  wait
  }
fi

if [ "${PHASE:-all}" = all ] || [ "$PHASE" = on ]; then
  # shellcheck disable=SC2086
  {
  SHOT="EXTRA_INI=dump_at = 150"
  for s in 0 1 2 3; do
    throttle; run b "S-g-s$s" $EM $GE "$SHOT" CFG="scale_gba = $s\nfilter_gba = 2" &
  done
  throttle; run b S-g-s1-amb $EM HERO="$HERO_EM" $GE CFG="scale_gba = 1\nfilter_gba = 2\nambient_gba = 1" &
  throttle; run b S-g-s0-amb $EM HERO="$HERO_EM" $GE CFG="scale_gba = 0\nfilter_gba = 2\nambient_gba = 1" &
  for s in 0 1 2 3; do
    throttle; run b "S-c-s$s" $CR $GE "$SHOT" CFG="console = 2\nscale_gbc = $s\nfilter_gbc = 2" &
  done
  throttle; run b S-c-s1-amb $CR HERO="$HERO_CR" $GE CFG="console = 2\nscale_gbc = 1\nfilter_gbc = 2\nambient_gbc = 1" &
  throttle; run b S-k-s1 $KB $KGE CFG="console = 1\nscale_gb = 1\nfilter_gb = 2" &
  # Nearest/bilinear twins for the GB/GBC side-by-sides and the GBC 2x identity.
  throttle; run b N-c-s1-f0 $CR $GE CFG="console = 2\nscale_gbc = 1\nfilter_gbc = 0" &
  throttle; run b N-c-s3-f0 $CR $GE CFG="console = 2\nscale_gbc = 3\nfilter_gbc = 0" &
  throttle; run b N-k-s1-f0 $KB $KGE CFG="console = 1\nscale_gb = 1\nfilter_gb = 0" &
  throttle; run b N-k-s1-f1 $KB $KGE CFG="console = 1\nscale_gb = 1\nfilter_gb = 1" &
  # The screenshot arm's off twin (frame_*.bmp must match).
  throttle; run b N-g-s1-f1-shot $EM $GE "$SHOT" CFG="scale_gba = 1\nfilter_gba = 1" &
  # Fast-forward: hold FF from frame 300 for 300 frames, every preset.
  FF="FRAMES=700 GEEVERY=7"
  throttle; run b S-ff-3x  $EM $FF "EXTRA_INI=simff = 300" CFG="scale_gba = 1\nfilter_gba = 2\nff_mult_x10 = 30" &
  throttle; run b S-ff-unl $EM $FF "EXTRA_INI=simff = 300" CFG="scale_gba = 1\nfilter_gba = 2\nff_mult_x10 = 0\nff_smooth = 0" &
  throttle; run b S-ff-smo $EM $FF "EXTRA_INI=simff = 300" CFG="scale_gba = 1\nfilter_gba = 2\nff_mult_x10 = 0\nff_smooth = 1" &
  wait
  }
fi

if [ "${PHASE:-all}" = all ] || [ "$PHASE" = perf ]; then
  # blit_prof / core_prof windows (600 frames each).  PPSSPP's GE is instant,
  # so `wait` means nothing here; `gu` (list building, real CPU work) and
  # `core` are what this can show.  One at a time, so runs do not share CPUs.
  # shellcheck disable=SC2086
  {
  P="FRAMES=1900"
  JOBS=1
  for f in 0 1 2; do
    throttle; run b "P-g-s1-f$f" $EM $P CFG="scale_gba = 1\nfilter_gba = $f" &
  done
  for f in 1 2; do
    throttle; run b "P-c-s2-f$f" $CR $P CFG="console = 2\nscale_gbc = 2\nfilter_gbc = $f" &
  done
  wait
  }
fi
