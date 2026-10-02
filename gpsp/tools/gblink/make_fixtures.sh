#!/usr/bin/env bash
# make_fixtures.sh BIN ROMDIR OUTDIR [GEN1ROM] -- build the GB link test fixtures.
#
#   BIN     directory holding linkplay (tools/gblink/build.sh BIN)
#   ROMDIR  holds the Gen 1 cartridge, crystal.gbc and crystal-owner.sav
#           (the owner's Crystal save, F: backup)
#   OUTDIR  receives gen1-a.sav, gen1-b.sav, gen2-a.sav, gen2-b.sav,
#           gen1-rom.sha1 (the cartridge the Gen 1 saves belong to) and
#           gen2-a.st, gen2-b.st: each player's RUNNING game (gbcore save
#           states), standing at the Cable Trade Center receptionist -- the
#           live-link fixtures (scripts/gen2_counter_*.ap)
#   GEN1ROM the Gen 1 cartridge in ROMDIR (default blue.gb).  A Gen 1 save
#           holds ROM pointers (the current map's data, text and script
#           pointers), so it is valid only on the cartridge it was made on:
#           the Blue saves loaded into Red draw a garbled map and never
#           reach the Cable Club (hardware run hw1, 2026-09-28; reproduced in
#           linkplay at the same frame, 7359).  Make them on the cartridge
#           the rig runs; setup_gblink_cards.py checks gen1-rom.sha1.
#
# Gen 1: no usable Blue/Red save exists in the owner's backups, so both are
# made here, deterministically: a new game played to Red's bedroom by the
# autopilot, one frame of RAM from gen1_fixture_pokes.py (names, IDs, one
# Pokemon each, the Pokedex events, a Fly warp to Viridian City), then the
# game walks to the Cable Club counter and saves.  Each run of this script
# produces byte-identical files.
#
# Gen 2 uses the owner's Crystal save from the F: backup (PSP-3000,
# 2026-09-27) as side a, and a copy re-branded by pksav.py as side b.
set -euo pipefail
BIN="$1"; ROMS="$2"; OUT="$3"; GEN1="${4:-blue.gb}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
cat "$HERE/scripts/gen1_fixture_newgame.ap" "$HERE/scripts/gen1_fixture_save.ap" \
  > "$OUT/gen1_fixture.ap"
python3 "$HERE/gen1_fixture_pokes.py" 0 3950 a > "$OUT/gen1_pokes.txt"
python3 "$HERE/gen1_fixture_pokes.py" 1 3950 b | grep -v '^#' >> "$OUT/gen1_pokes.txt"
"$BIN/linkplay" rom0="$ROMS/$GEN1" rom1="$ROMS/$GEN1" link=0 \
  ap0="$OUT/gen1_fixture.ap" ap1="$OUT/gen1_fixture.ap" \
  pokes="$OUT/gen1_pokes.txt" max=12000 after=120 \
  out0="$OUT/gen1-a.sav" out1="$OUT/gen1-b.sav" > "$OUT/gen1_fixture.log"
grep -E "ap_fail|lp_end|lp_save|name=(map|yx)" "$OUT/gen1_fixture.log"
python3 "$HERE/pksav.py" show "$OUT/gen1-a.sav" gen1
python3 "$HERE/pksav.py" show "$OUT/gen1-b.sav" gen1
cp "$ROMS/crystal-owner.sav" "$OUT/gen2-a.sav"
python3 "$HERE/pksav.py" rebrand "$OUT/gen2-a.sav" "$OUT/gen2-b.sav" gen2 LINKB 54321 2=GIFTSHREW
python3 "$HERE/pksav.py" show "$OUT/gen2-b.sav" gen2
# The live-link fixtures: both games played from the saves to the Cable
# Trade Center receptionist (no cable), and their states taken there.
"$BIN/linkplay" rom0="$ROMS/crystal.gbc" rom1="$ROMS/crystal.gbc" link=0 \
  sav0="$OUT/gen2-a.sav" sav1="$OUT/gen2-b.sav" rtc=1790500000 delay=0 \
  ap0="$HERE/scripts/gen2_counter_host.ap" ap1="$HERE/scripts/gen2_counter_guest.ap" \
  max=8000 stout0="$OUT/gen2-a.st" stout1="$OUT/gen2-b.st" > "$OUT/gen2_counter.log"
grep -E "ap_fail|lp_end" "$OUT/gen2_counter.log"
sha1sum < "$ROMS/$GEN1" | cut -c1-40 > "$OUT/gen1-rom.sha1"
(cd "$OUT" && md5sum gen1-a.sav gen1-b.sav gen2-a.sav gen2-b.sav gen2-a.st gen2-b.st && cat gen1-rom.sha1)
