#!/usr/bin/env bash
# run_trades.sh BIN ROMDIR FIXDIR OUTDIR [DELAY] -- the M2 acceptance run:
# a Gen 1 (Blue <-> Blue) and a Gen 2 (Crystal <-> Crystal) trade between
# the two machines of one link pair, driven by the scripts in scripts/,
# with the given input delay (frames between a script's buttons and the
# frame they are pressed in; a network session's delay).  Each trade is
# scored from the saves the games wrote (score_trade.py).  The sync-hash
# trace (every 60 frames) is kept as the golden for network runs.
#
# Each trade is run three times: both machines drawn, then as the HOST's
# console runs it (slot 1 headless) and as the GUEST's does (slot 0
# headless).  The sync-hash traces and the saves must be identical all
# three ways -- which is what two consoles in a session rely on.
set -uo pipefail
BIN="$1"; ROMS="$2"; FIX="$3"; OUT="$4"; DELAY="${5:-0}"
HERE="$(cd "$(dirname "$0")" && pwd)"
RTC=1790500000
mkdir -p "$OUT"
rc=0

one() {  # gen rom tag headless
  "$BIN/linkplay" rom0="$ROMS/$2" rom1="$ROMS/$2" \
    sav0="$FIX/$1-a.sav" sav1="$FIX/$1-b.sav" \
    ap0="$HERE/scripts/${1}_host.ap" ap1="$HERE/scripts/${1}_guest.ap" \
    delay="$DELAY" rtc=$RTC max=30000 after=60 headless="$4" \
    hashlog="$OUT/$3.hashes" out0="$OUT/$3-a.sav" out1="$OUT/$3-b.sav" \
    > "$OUT/$3.log"
}

run() {  # gen rom a_slot b_slot
  local gen=$1 rom=$2 sa=$3 sb=$4 tag="$1-d$DELAY" lp v
  one "$gen" "$rom" "$tag" 0
  lp=$?
  printf '%-10s linkplay=%d  %s\n' "$tag" "$lp" \
    "$(grep -o 'frames=[0-9/]* status=[-0-9/]* serial=[0-9/]* sync=[0-9a-f]*' "$OUT/$tag.log")"
  grep -h "ap_fail" "$OUT/$tag.log" | sed 's/^/    /'
  python3 "$HERE/score_trade.py" "$gen" "$FIX/$gen-a.sav" "$sa" "$FIX/$gen-b.sav" "$sb" \
    "$OUT/$tag-a.sav" "$OUT/$tag-b.sav" | sed 's/^/    /'
  python3 "$HERE/score_trade.py" "$gen" "$FIX/$gen-a.sav" "$sa" "$FIX/$gen-b.sav" "$sb" \
    "$OUT/$tag-a.sav" "$OUT/$tag-b.sav" >/dev/null || lp=1
  for v in 1 2; do
    one "$gen" "$rom" "$tag-h$v" "$v" || lp=1
    if cmp -s "$OUT/$tag.hashes" "$OUT/$tag-h$v.hashes" &&
       cmp -s "$OUT/$tag-a.sav" "$OUT/$tag-h$v-a.sav" &&
       cmp -s "$OUT/$tag-b.sav" "$OUT/$tag-h$v-b.sav"; then
      echo "    slot $((v - 1)) headless: $(wc -l < "$OUT/$tag.hashes") sync hashes and both saves identical to the drawn run"
    else
      echo "    slot $((v - 1)) headless: DIVERGED from the drawn run"
      lp=1
    fi
  done
  [ "$lp" = 0 ] || rc=1
}

run gen1 blue.gb 0 0
run gen2 crystal.gbc 0 2
exit $rc
