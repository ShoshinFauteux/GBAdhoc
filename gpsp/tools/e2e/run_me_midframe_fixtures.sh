#!/usr/bin/env bash
# run_me_midframe_fixtures.sh -- the ME mid-frame fix on REAL games, desktop.
#
# For each fixture, three arms through the ME_TIMING_SIM twin, capture mode 2
# (the CPU renders too, so every frame has a reference):
#   base   a twin built from the pre-fix contract (MMF_BASE_SDL: a gpsp_sdl
#          built with -DME_AFFINE_LINES=0 -DME_MIDFRAME_LOG=0, or from 3.0.0)
#   off    this tree, --me-midlog 0     (per-line affine only)
#   on     this tree, --me-midlog 1     (per-line affine + mid-frame log)
# and, for the `on` arm, the production mode (--me-capture-mode1) whose model
# images must equal the mode-2 CPU images.
#
# What it checks, per fixture:
#   cpu_same   the CPU images are identical in every arm: the fix does not
#              change emulation (the log's undo/redo runs on live arrays in
#              the model, so this also catches a log that fails to restore)
#   clean_eq   on frames the base engine already drew right, the new engines
#              draw the identical image (no regression on clean frames)
#   vis_bad    frames the modelled engine draws differently from the CPU
#   drift      replays that left VRAM/OAM/palette changed (must be 0)
#
# Usage (WSL):  tools/e2e/run_me_midframe_fixtures.sh [fixture ...]
#   fixtures: heart_soul_heavy heart_soul_light unbound_rival_medium
#             unbound_double_medium mgat_shot
# Needs run_me_midframe.sh to have built $MMF_WORK/src first.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"
MAIN=$SRC
while [ ! -d "$MAIN/builds/unbound-soak" ] && [ "$MAIN" != / ]; do MAIN=$(dirname "$MAIN"); done
WORK=${MMF_WORK:-$HOME/me-midframe}
NEW=$WORK/src/sdl
BASE=${MMF_BASE_SDL:-$HOME/me-midframe-base/src/sdl}
FIX=$MAIN/builds/unbound-soak/fixtures
ROMS=$MAIN/builds/unbound-harness/fixtures
PKINPUTS=$MAIN/builds/opus-perf-harness/campaign-psp3000-split/jobs/rend_r01_heart_soul_heavy_me1/battle.inputs
MGROM=${MMF_MGAT_ROM:-$WORK/roms/mgat.gba}
MGSAV=${MMF_MGAT_SAV:-$WORK/roms/mgat.sav}
FRAMES=${MMF_FIX_FRAMES:-3900}

run() {  # bin outdir mode-args...
  local bin=$1 o=$2; shift 2
  mkdir -p "$o"
  cp "$SAVE" "$o/game.sav"
  local st=()
  [ -n "$STATE" ] && { cp "$STATE" "$o/state.st0"; st=(--state "$o/state.st0"); }
  (cd "$bin" && SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 1800 ./gpsp_sdl \
    --rom "$ROM" --bios-dir "$BIOS" --save "$o/game.sav" "${st[@]}" \
    --script "$INPUTS" "$@" --ff --no-pacing --autoexit "$FRAMES" \
    --log "$o/frontend.log" --dump-dir "$o" 2>"$o/mts.log") || true
}

printf '%-22s %6s %8s %8s %8s %8s %8s %6s %6s\n' fixture frames cpu_same clean_eq \
  base_bad off_bad on_bad m1_bad drift
for fx in ${*:-heart_soul_heavy heart_soul_light unbound_rival_medium mgat_shot}; do
  case $fx in
    heart_soul*) ROM="$ROMS/Pokemon Heart & Soul.gba"; SAVE="$FIX/Pokemon Heart & Soul.sav"
                 STATE="$FIX/$fx.st0"; INPUTS=$PKINPUTS; BIOS=$ROMS;;
    unbound*)    ROM="$ROMS/Pokemon Unbound.gba"; SAVE="$FIX/Pokemon Unbound.sav"
                 STATE="$FIX/$fx.st0"; INPUTS=$PKINPUTS; BIOS=$ROMS;;
    mgat_shot)   ROM=$MGROM; SAVE=$MGSAV; STATE=""; BIOS=$(dirname "$MGROM")
                 INPUTS=$SCRIPT_DIR/me_midframe/mgat_shot.inputs;;
    *) echo "unknown fixture $fx" >&2; continue;;
  esac
  O=$WORK/fixtures/$fx; rm -rf "$O"
  run "$BASE" "$O/base" --me-capture-test
  run "$NEW"  "$O/off"  --me-capture-test --me-midlog 0
  run "$NEW"  "$O/on"   --me-capture-test --me-midlog 1
  run "$NEW"  "$O/m1"   --me-capture-mode1 --me-midlog 1
  python3 - "$O" "$fx" <<'PYEOF'
import re, sys
o, name = sys.argv[1], sys.argv[2]
pat = re.compile(r'^MTS f=(\d+) cpu=(\w+) post=(\w+) vis=(\w+)', re.M)
def rows(arm):
    t = open('%s/%s/mts.log' % (o, arm), errors='replace').read()
    d = re.search(r'MTS_DRIFT_TOTAL (\d+)', t)
    return {int(f): (c, v) for f, c, p, v in pat.findall(t)}, int(d.group(1)) if d else -1
b, _ = rows('base'); f, d1 = rows('off'); n, d2 = rows('on'); m, d3 = rows('m1')
fr = sorted(k for k in b if k > 30 and k in f and k in n)
cpu_same = all(b[k][0] == f[k][0] == n[k][0] for k in fr)
clean = [k for k in fr if b[k][1] == b[k][0]]
clean_eq = sum(1 for k in clean if f[k][1] == b[k][1] and n[k][1] == b[k][1])
bad = lambda a: sum(1 for k in fr if a[k][1] != a[k][0])
m1bad = sum(1 for k in fr if k not in m or m[k][1] != n[k][0])
print('%-22s %6d %8s %4d/%-4d %8d %8d %8d %6d %6d' % (name, len(fr), 'yes' if cpu_same else 'NO',
      clean_eq, len(clean), bad(b), bad(f), bad(n), m1bad, max(d1, d2, d3)))
PYEOF
done
