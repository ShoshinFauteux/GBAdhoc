#!/usr/bin/env bash
# p_link.sh OUT HOST_SCRIPT JOIN_SCRIPT FRAMES [JITTER_MS] [LOSS_PCT] [LATENCY_MS]
# Design P probe: two SDL-twin instances of AW2 linked through gpSP's own
# Advance Wars serial-protocol emulation (serial_proto.c, SERIAL_MODE_SERIAL_AW2)
# over the netdrv UDP transport, in a private network namespace.
set -u
OUT=$1; HS=$2; JS=$3; FR=$4; JIT=${5:-0}; LOSS=${6:-0}; LAT=${7:-0}
SDL=${SDL:-$(dirname "$0")/gpsp_sdl_lb}
ROM=${ROM:-$HOME/gbalink/roms/aw2.gba}; ST=${ST:-$HOME/gbalink/roms/aw2.st0}
SAV=${SAV:-$HOME/gbalink/roms/aw2.sav}; BIOS=$HOME/gbalink/bios
mkdir -p "$OUT/h" "$OUT/j"; rm -f "$OUT"/h/* "$OUT"/j/*
cp "$SAV" "$OUT/h.sav"; cp "$SAV" "$OUT/j.sav"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
COMMON="--rom $ROM --bios-dir $BIOS --state $ST --autoexit $FR --net-jitter $JIT --net-loss $LOSS --net-latency $LAT"
unshare -rn bash -c "ip link set lo up
  $SDL $COMMON --save $OUT/h.sav --script $HS --host 27400 --nick HOST --log $OUT/h.log --dump-dir $OUT/h --dump-every 120 > $OUT/h.out 2>&1 &
  sleep 0.5
  $SDL $COMMON --save $OUT/j.sav --script $JS --join 127.0.0.1:27400 --nick JOIN --log $OUT/j.log --dump-dir $OUT/j --dump-every 120 > $OUT/j.out 2>&1 &
  wait"
grep -hE "exit|np_|session" "$OUT/h.log" | tail -3
