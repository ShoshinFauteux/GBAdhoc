# G2 validation chain on PPSSPP.  Each line: tag | env | args.  Results in ~/wireless-rig/g2/<tag>.dir
cd ~/wireless-rig
mkdir -p g2
export SANDBOX_ROOT=~/gpsp-e2e/wireless-sb TIMEOUT_S=2700 TURNS=${TURNS:-20}
GROWL_H=~/wireless-rig/golden/growl/host-firered.sav
GROWL_J=~/wireless-rig/golden/growl/join-leafgreen.sav
run() { # tag arm hostextra joinextra [hostsav joinsav joinrom]
  local tag=$1 arm=$2
  export HOST_EXTRA_INI="$3" JOIN_EXTRA_INI="$4"
  export HOST_SAV=${5:-$GROWL_H} JOIN_SAV=${6:-$GROWL_J}
  if [ -n "${7:-}" ]; then export JOIN_ROM="$7"; else unset JOIN_ROM; fi
  export HOST_SCRIPT="${HOST_SCRIPT:-frlg_battle_host.inputs}" JOIN_SCRIPT="${JOIN_SCRIPT:-frlg_battle_join.inputs}"
  local A
  A=$(bash tools/e2e/run_battle_rig_psp.sh --arm=$arm 2>&1 | tail -1)
  echo "$A" > g2/$tag.dir
  echo "$(date +%T) $tag -> $A" >> g2/chain.log
}
for step in "$@"; do
  case $step in
    nullB)   run nullB B "" "" ;;
    nullB2)  run nullB2 B "" "" ;;
    nullA)   run nullA A "" "" ;;
    latA)    run latA A "net_latency_ms = 50" "net_latency_ms = 50" ;;
    latB)    run latB B "net_latency_ms = 50" "net_latency_ms = 50" ;;
    slowA)   run slowA A "" "pace_slow_us = 13000" ;;
    slowB)   run slowB B "" "pace_slow_us = 13000" ;;
    slowA2)  run slowA2 A "" "pace_slow_us = 6000" ;;
    slowB2)  run slowB2 B "" "pace_slow_us = 6000" ;;
    slowA3)  run slowA3 A "" "pace_slow_us = 9500" ;;  # 6000 did not ratchet, 13000 collapsed in setup
    slowB3)  run slowB3 B "" "pace_slow_us = 9500" ;;
    fault)   run fault B "" "rfu_fault_mode = 1
rfu_fault_corrupt = 4" ;;  # the 4th SETMONDATA value (turn 1 has four PP updates)
    faultspan) run faultspan B "" "rfu_fault_corrupt = 1500
rfu_fault_span = 48" ;;  # generic flips, mostly display-only data
    # HOLD validation (rfu_hold, 57c691b): bursts from ARQ loss recovery and
    # head-of-line jitter on BOTH receive paths, the H0b run 3 shape.
    burstA)  run burstA A "net_loss_pct = 8
net_jitter_ms = 120" "net_loss_pct = 8
net_jitter_ms = 120" ;;
    burstB0) run burstB0 B "net_loss_pct = 8
net_jitter_ms = 120
rfu_hold = 0" "net_loss_pct = 8
net_jitter_ms = 120
rfu_hold = 0" ;;
    burstB)  run burstB B "net_loss_pct = 8
net_jitter_ms = 120" "net_loss_pct = 8
net_jitter_ms = 120" ;;
    # Milder: burst* (8 % / 120 ms) measured 20-28 % air loss and the
    # transport itself outlived the game's 32-frame link timeout in every arm.
    # H0b run 3 had 2-5 % air loss.
    burst2A)  run burst2A A "net_loss_pct = 3
net_jitter_ms = 40" "net_loss_pct = 3
net_jitter_ms = 40" ;;
    burst2B0) run burst2B0 B "net_loss_pct = 3
net_jitter_ms = 40
rfu_hold = 0" "net_loss_pct = 3
net_jitter_ms = 40
rfu_hold = 0" ;;
    burst2B)  run burst2B B "net_loss_pct = 3
net_jitter_ms = 40" "net_loss_pct = 3
net_jitter_ms = 40" ;;
    # Timed-hold walk validation (1582488): the overshoot cases.
    walkB)   run walkB B "" "" ;;
    walkL)   run walkL B "net_latency_ms = 50" "net_latency_ms = 50" ;;
    walkLA)  run walkLA A "net_latency_ms = 50" "net_latency_ms = 50" ;;
    walkS)   run walkS B "" "pace_slow_us = 9500" ;;
    # Radio FADE (ff4fe7f): 1.5 s of total receive loss every 20 s on both
    # consoles -- the hardware's 1-2 s ARQ stall, then a burst release.
    fadeA)   run fadeA A "net_blackout_ms = 1500" "net_blackout_ms = 1500" ;;
    fadeB0)  run fadeB0 B "net_blackout_ms = 1500
rfu_hold = 0" "net_blackout_ms = 1500
rfu_hold = 0" ;;
    fadeB)   run fadeB B "net_blackout_ms = 1500" "net_blackout_ms = 1500" ;;
    io42)    run io42 B "io_prio = 42" "io_prio = 42" ;;
    swiftA)  TURNS=12 run swiftA A "" "" ~/wireless-rig/golden/swift/host-firered.sav ~/wireless-rig/golden/swift/join-leafgreen.sav ;;
    swiftB)  TURNS=12 run swiftB B "" "" ~/wireless-rig/golden/swift/host-firered.sav ~/wireless-rig/golden/swift/join-leafgreen.sav ;;
    frfrB)   run frfrB B "" "" ~/wireless-rig/golden/frfr/host-firered.sav ~/wireless-rig/golden/frfr/join-firered.sav "$HOME/wireless-rig/testdata/Pokemon - FireRed Version (USA, Europe) (Rev 1).gba" ;;
    emB)     HOST_SCRIPT=emerald_battle_host.inputs JOIN_SCRIPT=emerald_battle_join.inputs HOST_ROM="$HOME/wireless-rig/testdata/Pokemon - Emerald Version (USA, Europe).gba" run emB B "" "" ~/wireless-rig/golden/emerald/host-emerald.sav ~/wireless-rig/golden/emerald/join-emerald.sav "$HOME/wireless-rig/testdata/Pokemon - Emerald Version (USA, Europe).gba" ;;
  esac
done
echo "$(date +%T) chain done" >> g2/chain.log
