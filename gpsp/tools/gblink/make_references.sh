#!/usr/bin/env bash
# make_references.sh BIN ROMDIR FIXDIR OUT [DELAY] -- the one-process results
# the hardware rig's arms must reproduce byte for byte.
#
#   BIN     linkplay (tools/gblink/build.sh BIN)
#   ROMDIR  red.gb (the rig's Gen 1 cartridge) and crystal.gbc
#   FIXDIR  make_fixtures.sh's output for that cartridge (gen1-*, gen2-*,
#           gen1-rom.sha1)
#   OUT     receives gen1-d4-{a,b}.sav, gen2-d4-{a,b}.sav, battle-d4-{a,b}.sav,
#           and live-d4-{a,b}.sav + live-d4.final (frame:hash): the LIVE
#           link from the gen2-*.st states (sesssim, the whole session stack:
#           its end frame follows the host script's `disconnect`)
#
# A two-console session reproduces these exactly when both consoles' games
# start from the same saves and the same clock: the harness freezes the
# cartridge clock at gblink_rtc_seed (main_psp.c) and linkplay starts its
# emulated clock there too.  Each arm is also checked by the exact trade /
# battle oracle, so a reference mismatch is a second, independent alarm.
set -euo pipefail
BIN="$1"; ROMS="$2"; FIX="$3"; OUT="$4"; DELAY="${5:-4}"
HERE="$(cd "$(dirname "$0")" && pwd)"
RTC=1790500000
mkdir -p "$OUT"
one() {  # tag rom save-prefix script-prefix
  "$BIN/linkplay" rom0="$ROMS/$2" rom1="$ROMS/$2" \
    sav0="$FIX/$3-a.sav" sav1="$FIX/$3-b.sav" \
    ap0="$HERE/scripts/${4}_host.ap" ap1="$HERE/scripts/${4}_guest.ap" \
    delay="$DELAY" rtc=$RTC max=40000 after=60 \
    out0="$OUT/$1-d$DELAY-a.sav" out1="$OUT/$1-d$DELAY-b.sav" > "$OUT/$1-d$DELAY.log"
  grep -o 'lp_end.*' "$OUT/$1-d$DELAY.log"
}
one gen1 red.gb gen1 gen1
one gen2 crystal.gbc gen2 gen2
one battle red.gb gen1 gen1_battle
"$BIN/sesssim" rom_h="$ROMS/crystal.gbc" rom_g="$ROMS/crystal.gbc" \
  sav_h="$FIX/gen2-a.sav" sav_g="$FIX/gen2-b.sav" \
  st_h="$FIX/gen2-a.st" st_g="$FIX/gen2-b.st" \
  ap_h="$HERE/scripts/gen2_live_host.ap" ap_g="$HERE/scripts/gen2_live_guest.ap" \
  delay="$DELAY" lat=2 rtc=$RTC max=40000 bulk=1037 \
  out_h="$OUT/live-d$DELAY-a.sav" out_g="$OUT/live-d$DELAY-b.sav" > "$OUT/live-d$DELAY.log"
grep -h "gblink_done" "$OUT/live-d$DELAY.log" | head -1 |
  sed -n 's/.*frame=\([0-9]*\) hash=\([0-9a-f]*\).*/\1:\2/p' > "$OUT/live-d$DELAY.final"
echo "live: $(cat "$OUT/live-d$DELAY.final")"
cp "$FIX/gen1-rom.sha1" "$OUT/"
(cd "$OUT" && md5sum ./*.sav)
