#!/usr/bin/env bash
# run_sessions.sh BIN ROMDIR FIXDIR OUTDIR -- the M3 acceptance run: whole
# link SESSIONS (fe_gblink, both consoles, sesssim) over a simulated
# network, for the Gen 1 and Gen 2 trades.
#
# For every input delay D and network profile (latency / jitter in frames)
# both consoles must end DONE with every periodic hash matched, and the
# saves each console commits must be byte-identical to the one-process
# golden (linkplay) at the same D: the network only moves WHEN frames run,
# never what they compute.  Jitter shows up as stalls, which are counted.
# (Compared: the cartridge RAM.  A clock cartridge's 48-byte trailer is a
# snapshot of the clock at the moment the image is read; the session reads
# it at its agreed end frame, linkplay at its own last frame.)
#
# Then the failure paths, which must write NOTHING:
#   desync   one byte of the guest console's copy of the host's Game Boy is
#            flipped mid-session -> both consoles FAILED/desync, no save.
#   transfer the guest has no copy of the host's cartridge (and vice
#            versa): both are sent in memory, SHA-1 checked, then the trade
#            runs and must still equal the golden.
set -uo pipefail
BIN="$1"; ROMS="$2"; FIX="$3"; OUT="$4"
HERE="$(cd "$(dirname "$0")" && pwd)"
S="$HERE/scripts"
RTC=1790500000
mkdir -p "$OUT/lib-empty"
rc=0

same_ram() {  # a b: equal cartridge RAM (the image minus any clock trailer)
  python3 - "$1" "$2" <<'PY'
import sys
a, b = (open(p, 'rb').read() for p in sys.argv[1:3])
cut = lambda d: d[:len(d) - 48] if len(d) % 256 == 48 else d
sys.exit(0 if len(a) == len(b) and cut(a) == cut(b) else 1)
PY
}

golden() {  # gen rom delay -> $OUT/golden-gen-dD-{a,b}.sav
  local g="$OUT/golden-$1-d$3"
  [ -f "$g-a.sav" ] && return 0
  "$BIN/linkplay" rom0="$ROMS/$2" rom1="$ROMS/$2" sav0="$FIX/$1-a.sav" \
    sav1="$FIX/$1-b.sav" ap0="$S/${1}_host.ap" ap1="$S/${1}_guest.ap" \
    delay="$3" rtc=$RTC max=30000 after=60 out0="$g-a.sav" out1="$g-b.sav" \
    > "$g.log" || { echo "golden $1 d$3 FAILED"; rc=1; }
}

session() {  # tag gen rom delay lat jitter [extra...]
  local tag=$1 gen=$2 rom=$3 d=$4 lat=$5 jit=$6; shift 6
  "$BIN/sesssim" rom_h="$ROMS/$rom" rom_g="$ROMS/$rom" sav_h="$FIX/$gen-a.sav" \
    sav_g="$FIX/$gen-b.sav" ap_h="$S/${gen}_host.ap" ap_g="$S/${gen}_guest.ap" \
    delay="$d" lat="$lat" jitter="$jit" rtc=$RTC max=400000 \
    out_h="$OUT/$tag-h.sav" out_g="$OUT/$tag-g.sav" "$@" > "$OUT/$tag.log"
}

summary() {  # tag -> one line from the result events
  awk '/EVT result/ { for (i = 1; i <= NF; i++) if ($i ~ /^(console|state|committed|stalls|streak_max|hashes)=/) printf "%s ", $i; printf "| " }' "$OUT/$1.log"
}

for gen in gen1 gen2; do
  rom=blue.gb; [ $gen = gen2 ] && rom=crystal.gbc
  for d in 2 4 8; do
    golden $gen $rom $d
    for net in "1 0" "3 6" "6 20"; do
      set -- $net
      tag="$gen-d$d-lat$1-jit$2"
      rm -f "$OUT/$tag-h.sav" "$OUT/$tag-g.sav"
      session "$tag" $gen $rom $d $1 $2
      src=$?
      if [ $src = 0 ] && same_ram "$OUT/$tag-h.sav" "$OUT/golden-$gen-d$d-a.sav" &&
         same_ram "$OUT/$tag-g.sav" "$OUT/golden-$gen-d$d-b.sav"; then
        v=PASS
      else
        v=FAIL; rc=1
      fi
      printf '%-4s %-22s %s\n' "$v" "$tag" "$(summary "$tag")"
    done
  done
done

# desync: must fail on both consoles and write nothing
tag=gen1-desync
rm -f "$OUT/$tag-h.sav" "$OUT/$tag-g.sav"
session "$tag" gen1 blue.gb 4 2 0 desync_at=1500
if grep -q "gblink_desync" "$OUT/$tag.log" && [ ! -e "$OUT/$tag-h.sav" ] &&
   [ ! -e "$OUT/$tag-g.sav" ] && grep -c "state=4" "$OUT/$tag.log" | grep -q 2; then
  printf 'PASS %-22s %s\n' "$tag" "$(grep -o 'gblink_desync frame=[0-9]*' "$OUT/$tag.log" | head -1); no save written by either console"
else
  printf 'FAIL %-22s %s\n' "$tag" "$(summary "$tag")"; rc=1
fi

# transfer: Gen 2 trade where neither console has the other's cartridge
# file (both are Crystal, so present one under a different name and SHA-1:
# the guest plays a copy with one padding byte changed).
if [ -f "$ROMS/crystal.gbc" ]; then
  python3 - "$ROMS/crystal.gbc" "$OUT/crystal-guest.gbc" <<'PY'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
d[-1] ^= 0x01            # the last byte of the last bank: unused padding
open(sys.argv[2], 'wb').write(bytes(d))
PY
  golden2="$OUT/golden-gen2-xfer"
  "$BIN/linkplay" rom0="$ROMS/crystal.gbc" rom1="$OUT/crystal-guest.gbc" \
    sav0="$FIX/gen2-a.sav" sav1="$FIX/gen2-b.sav" ap0="$S/gen2_host.ap" \
    ap1="$S/gen2_guest.ap" delay=4 rtc=$RTC max=30000 after=60 \
    out0="$golden2-a.sav" out1="$golden2-b.sav" > "$golden2.log"
  tag=gen2-transfer
  "$BIN/sesssim" rom_h="$ROMS/crystal.gbc" rom_g="$OUT/crystal-guest.gbc" \
    sav_h="$FIX/gen2-a.sav" sav_g="$FIX/gen2-b.sav" ap_h="$S/gen2_host.ap" \
    ap_g="$S/gen2_guest.ap" delay=4 lat=3 jitter=6 rtc=$RTC max=400000 \
    out_h="$OUT/$tag-h.sav" out_g="$OUT/$tag-g.sav" > "$OUT/$tag.log"
  if [ $? = 0 ] && same_ram "$OUT/$tag-h.sav" "$golden2-a.sav" &&
     same_ram "$OUT/$tag-g.sav" "$golden2-b.sav" &&
     [ "$(grep -c 'gblink_rom_received size=2097152 ok=1' "$OUT/$tag.log")" = 2 ]; then
    printf 'PASS %-22s both cartridges sent and SHA-1 checked; %s\n' "$tag" "$(summary "$tag")"
  else
    printf 'FAIL %-22s %s\n' "$tag" "$(summary "$tag")"; rc=1
  fi
  python3 "$HERE/score_trade.py" gen2 "$FIX/gen2-a.sav" 0 "$FIX/gen2-b.sav" 2 \
    "$OUT/$tag-h.sav" "$OUT/$tag-g.sav" | sed 's/^/     /'
fi
# clock skew: the guest console's clock 17 minutes ahead of the host's.
# The session re-bases both saves' clocks to the host's, so the consoles
# still agree on every frame (the saves need not equal the golden: the
# guest's game clock really has advanced 17 minutes further).
tag=gen2-skew
session "$tag" gen2 crystal.gbc 4 2 2 skew=1020
if [ $? = 0 ] && grep -q "gblink_done" "$OUT/$tag.log"; then
  printf 'PASS %-22s consoles agree with clocks 17 min apart; %s\n' "$tag" "$(summary "$tag")"
  python3 "$HERE/score_trade.py" gen2 "$FIX/gen2-a.sav" 0 "$FIX/gen2-b.sav" 2 \
    "$OUT/$tag-h.sav" "$OUT/$tag-g.sav" | sed 's/^/     /'
else
  printf 'FAIL %-22s %s\n' "$tag" "$(summary "$tag")"; rc=1
fi
exit $rc
