#!/usr/bin/env bash
# survey.sh BUILD -- which games write their cartridge save, and how often.
# Every ROM of the dynarec corpus, 14400 frames (4 minutes) of START/A/dpad
# mashing from power-on, with the owner's .sav when the main checkout has
# one.  Reads the rewind_win lines (save_frames = frames with a save write,
# closed_frames = frames the history was shut because of a save).
set -uo pipefail
BUILD=${1:-d4}
W=$HOME/rewind-agent
ROMS=$HOME/drcorpus/corpus/roms
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/workdirs.sh"
SAVS=${RW_SAVS:-$GBADHOC_ROMS}
OUT=$W/survey; rm -rf "$OUT"; mkdir -p "$OUT"
S=$OUT/mash.inputs
{
  echo "wait 200"
  for i in $(seq 1 140); do
    echo "press START 4"; echo "wait 30"; echo "press A 4"; echo "wait 30"
    case $((i % 4)) in 0) d=UP;; 1) d=RIGHT;; 2) d=DOWN;; *) d=LEFT;; esac
    echo "hold $d 20"; echo "press A 4"; echo "wait 10"
  done
  echo "wait 100000"
} > "$S"
run_one() {
  rom=$1; n=$(basename "$rom" .gba); d=$OUT/$n; mkdir -p "$d"
  # the owner's save for this title, if any (matched on the first two words)
  key=$(echo "$n" | sed 's/-[0-9a-f]*$//' | cut -d_ -f1-2 | tr _ ' ')
  sav=$(ls "$SAVS"/*.sav 2>/dev/null | grep -i "$(echo "$key" | sed 's/ /.*/')" | head -1)
  [ -n "$sav" ] && cp "$sav" "$d/game.sav" && echo "$sav" > "$d/sav_from.txt"
  ( cd "$W/$BUILD/sdl" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 900 ./gpsp_sdl \
      --rom "$rom" --bios-dir "$W/bios" --save "$d/game.sav" --script "$S" --ff --no-pacing \
      --log "$d/frontend.log" --dump-dir "$d" --autoexit 14400 \
      --rewind --rewind-inplace --rewind-dirty 3 --rewind-verify >"$d/err.log" 2>&1 )
  echo "$n rc=$?"
}
export -f run_one; export W ROMS SAVS OUT S BUILD
ls "$ROMS"/*.gba | xargs -P 8 -I{} bash -c 'run_one "{}"' > "$OUT/runs.txt" 2>&1
python3 - "$OUT" <<'EOF'
import os, re, sys
out = sys.argv[1]
rows = []
for n in sorted(os.listdir(out)):
    p = os.path.join(out, n, 'frontend.log')
    if not os.path.exists(p):
        continue
    wins = []; saves = '?'; verify = '?'; anomalies = '?'; btype = ''
    for line in open(p, errors='replace'):
        m = re.match(r'EVT rewind_win frames=(\d+).*history_frames=(\d+) save_frames=(\d+) closed_frames=(\d+) serial_frames=(\d+)', line)
        if m:
            wins.append(tuple(int(x) for x in m.groups()))
        m = re.search(r'save_barriers=(\d+)', line)
        if m: saves = m.group(1)
        m = re.search(r'anomalies=(\d+) selfcheck=(\d+) verify=(\d+)/(\d+)', line)
        if m: anomalies, verify = m.group(1), m.group(3) + '/' + m.group(4)
    late = wins[5:]      # after the first 3000 frames
    wr = sum(1 for w in late if w[2])
    closed = sum(w[3] for w in late); fr = sum(w[0] for w in late)
    hist = max((w[1] for w in late), default=0)
    ser = sum(1 for w in late if w[4]); serf = sum(w[4] for w in late)
    rows.append((n, saves, len(wins), wr, len(late), 100.0 * closed / fr if fr else 0, ser, serf, hist, anomalies, verify,
                 'sav' if os.path.exists(os.path.join(out, n, 'sav_from.txt')) else ''))
print('%-44s %5s %4s %12s %7s %6s %7s %9s %4s %10s %s' % ('rom', 'saves', 'wins', 'late_w/write', 'closed%', 'ser_w', 'ser_fr', 'max_hist', 'anom', 'verify', ''))
for r in rows:
    print('%-44s %5s %4d %7d/%-4d %6.1f%% %6d %7d %9d %4s %10s %s' % r)
EOF
