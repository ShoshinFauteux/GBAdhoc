#!/usr/bin/env bash
# batch_me_sleep.sh <harness eboot dir> [arms]: issue 28, docs/INPROC-RETURN.md section 8.  me_fake stands in for the module.
# Pass: a list sleep logs module=1 retained=1 handed_back=0.  a3/b2 are the mutant (handed_back=1), a4 the wedged engine (me_stopped).
HERE="$(cd "$(dirname "$0")" && pwd)"
ED="$1"; shift
export OUT="${OUT:-$HOME/inprocme-runs}" FX="${FX:-$HOME/drprof/fx}"
mkdir -p $OUT
# arm NAME ROMS CYCLES FRAMES EXTRA [CFG]
arm() { n=$1; ROMS=$2 CYCLES=$3 FRAMES=$4 AUTOEXIT=$(($4 + 300)) EXTRA="$5" CFG="${6:-}" bash $HERE/run142.sh $n "$ED" $HERE/setup_inproc.sh 240 >/dev/null 2>&1
  echo "=== $n"; grep -E 'EVT (sleep_sim|power |me_init|me_retain|me_rend (off|on)|inproc_(return|refused|census|session)|standby|exit)' $OUT/$n/frontend.log 2>/dev/null | cut -c1-130 | head -${6:-60}; }
arms="$*"
want() { case " $arms " in *" $1 "*|"  ") return 0;; esac; [ -z "$arms" ]; }
F='me_fake = 1\nme_boot = 1\nme_mode = 1'
S='sleep_sim_browser = 1\nsleep_sim_ms = 800'
want a1 && arm a1 'aw2.gba|red.gb|emerald.gba' 3 200 "$F"
want a2 && arm a2 'aw2.gba|emerald.gba' 2 200 "$F\n$S"
want a3 && arm a3 'aw2.gba|emerald.gba' 2 200 "$F\n$S\ninproc_me_unload = 1"
want a4 && arm a4 'aw2.gba|emerald.gba' 2 200 "me_fake = 2\nme_boot = 1\nme_mode = 1"
want a5 && arm a5 'aw2.gba|red.gb|emerald.gba' 3 200 "$F\n$S\nsleep_sim_game = 1"
want a6 && arm a6 'aw2.gba|emerald.gba' 2 200 "$S"
want a7 && arm a7 'aw2.gba|emerald.gba' 2 400 "$F\nsleep_sim_at = 120\nsleep_sim_ms = 800\nsleep_sim_choice = 1\nsleep_sim_game = 1"
want a8 && arm a8 'aw2.gba|emerald.gba' 2 400 "$F\nsleep_sim_at = 120\nsleep_sim_ms = 800\nsleep_sim_choice = 0\nsleep_sim_again_ms = 1500\nsleep_sim_choice_frame = 240\nsleep_sim_game = 1"
# through the real browser: the list is really up when the sleep lands
barm() { n=$1; SCRIPT=$2 CYCLES=$3 FRAMES=200 AUTOEXIT=500 EXTRA="$4" CFG="" bash $HERE/run142.sh $n "$ED" $HERE/setup_rbrowse.sh 240 >/dev/null 2>&1
  echo "=== $n"; grep -E 'EVT (sleep_sim|power |me_init|me_retain|me_rend (off|on)|inproc_(return|refused|census|session)|standby|exit)|browser (un)?park' $OUT/$n/frontend.log $OUT/$n/standby.log 2>/dev/null | sed 's/^.*frontend.log://;s/^.*standby.log://' | cut -c1-120 | head -60; }
P='60 press CROSS\n420 press CROSS'
want b1 && barm b1 "$P" 2 "$F\n$S"
want b2 && barm b2 "$P" 2 "$F\n$S\ninproc_me_unload = 1\ninproc_force = 1"
want b3 && barm b3 "$P" 2 "$S"
want b4 && barm b4 "$P" 2 "$F\n$S\nsleep_sim_again_ms = 500"
want b6 && barm b6 "$P" 2 "$F\n$S\nrewind = 1"
want b7 && barm b7 '60 press CROSS\n320 press DOWN\n340 press CROSS\n700 press CROSS' 3 "$F\n$S\nsleep_sim_game = 1"
