#!/usr/bin/env bash
# run_stage_pair.sh -- one arm of the GB link HARDWARE rig, run in two PPSSPP
# instances exactly as staged: the stage directory's EBOOT, host-/join-
# harness ini (minus the USB handoff keys, which need a real console),
# scripts and owner CONFIG.INI, the golden saves restored into roms/ the way
# hw_loop.py restores them, and the card's ROM names.  Then it scores the pair
# with score_session.py, the same scorer the hardware logs go through.
#
#   run_stage_pair.sh OUT RIG ARM ROMDIR [--fat=host|join|both] [--rom-red=FILE]
#     RIG     prepare_gblink_rig.py's output (stage-ARM/, golden/, scripts/,
#             cards/roms/synth8m.gbc, reference/, MANIFEST.txt)
#     ROMDIR  holds crystal.gbc and red.gb (the cartridge the rig's Gen 1
#             fixtures were made on; --rom-red=FILE names another file)
#     --host-key=K=V / --join-key=K=V  one more harness key on that side,
#             e.g. psp_clock=1298863731 (the PSP-1000's 2011 clock in hw1)
#
# Must run inside a private namespace (see run_psp_pair.sh).
set -uo pipefail
OUT="$1"; RIG="$2"; ARM="$3"; ROMS="$4"; shift 4
HERE="$(cd "$(dirname "$0")" && pwd)"
FAT=""; RED="$ROMS/red.gb"; XH=""; XJ=""
NL='
'
for a in "$@"; do
  case "$a" in
    --fat=*) FAT="${a#*=}" ;;
    --host-key=*) XH="$XH${a#*=}$NL" ;;
    --join-key=*) XJ="$XJ${a#*=}$NL" ;;
    --rom-red=*) RED="${a#*=}" ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SB="$HOME/gpsp-e2e/sandboxes/gbrig-$(basename "$OUT")"
TIMEOUT_S="${TIMEOUT_S:-1500}"
ST="$RIG/stage-$ARM"
[ -d "$ST" ] || { echo "no $ST" >&2; exit 2; }
rm -rf "$OUT" "$SB"; mkdir -p "$OUT"
note() { echo "$*" | tee -a "$OUT/verdict.txt"; }

setup() {  # inst role
  local MS="$SB/inst$1/ppsspp" srv=False model=1 f t
  local GAME="$MS/PSP/GAME/GBADHOC-GBLINK"
  [ "$1" = 1 ] && srv=True
  case "$FAT" in both|"$2") model=0 ;; esac
  mkdir -p "$MS/PSP/SYSTEM" "$GAME/roms" "$GAME/log" "$GAME/handoff"
  cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOF
[General]
FirstRun = False
CheckForNewVersion = False

[Graphics]
GraphicsBackend = 3 (VULKAN)

[Network]
EnableWlan = True
EnableAdhocServer = $srv
proAdhocServer = localhost
AdhocServerRelayMode = 2
PortOffset = 10000
EnableUPnP = False

[SystemParam]
NickName = GBRIG$1
MacAddress = 02:00:00:00:00:0$1
PSPModel = $model
EOF
  for f in "$ST"/* "$ST"/.[!.]*; do
    [ -f "$f" ] || continue
    t="$(basename "$f")"
    case "$t" in
      host-*|join-*) [ "${t%%-*}" = "$2" ] || continue; t="${t#*-}" ;;
    esac
    cp "$f" "$GAME/$t"
  done
  # The console-only handoff keys go; the arm's identity is appended as
  # hw_loop does.  CONFIG.INI keeps the owner's CRLF (as on the card).
  grep -v '^handoff' "$GAME/.gpsp-harness.ini" > "$GAME/h.tmp"
  printf '\nrun_id = ppsspp\narm = %s\n' "$ARM" >> "$GAME/h.tmp"
  if [ "$2" = host ]; then printf '%s' "$XH" >> "$GAME/h.tmp"
  else printf '%s' "$XJ" >> "$GAME/h.tmp"; fi
  mv "$GAME/h.tmp" "$GAME/.gpsp-harness.ini"
  mv "$GAME/CONFIG.INI" "$GAME/config.ini"
  cp "$ROMS/crystal.gbc" "$GAME/roms/crystal.gbc"
  cp "$RED" "$GAME/roms/red.gb"
  cp "$RIG/cards/roms/synth8m.gbc" "$GAME/roms/synth8m.gbc"
  for f in "$RIG/golden/$2-"*; do
    t="$(basename "$f")"; cp "$f" "$GAME/roms/${t#*-}"
  done
}
setup 1 host; setup 2 join
G1="$SB/inst1/ppsspp/PSP/GAME/GBADHOC-GBLINK"; G2="$SB/inst2/ppsspp/PSP/GAME/GBADHOC-GBLINK"
HL="$G1/log/frontend.log"; JL="$G2/log/frontend.log"
note "stage $ARM ($ST) fat=${FAT:-none} red=$(md5sum < "$RED" | cut -c1-8)"

rm -f /dev/shm/PPSSPP_ID
# SOFTGPU=1: PPSSPP's software renderer, so the GE's output really lands in
# emulated VRAM and the harness's screen dumps (gedump_at, gedump_loops) show
# what the screen shows; the hardware renderer leaves VRAM stale.
APPEND=""
if [ "${SOFTGPU:-0}" = 1 ]; then
  printf '[Graphics]\nSoftwareRenderer = True\n' > "$SB/softgpu.ini"
  APPEND="--appendconfig=$SB/softgpu.ini"
fi
launch() {
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SB/inst$1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 60)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed $APPEND "$SB/inst$1/ppsspp/PSP/GAME/GBADHOC-GBLINK/EBOOT.PBP" \
  ) > "$OUT/inst$1.emu.log" 2>&1 &
  echo $!
}
T0=$(date +%s)
PID1=$(launch 1)
# LAUNCH=together: both at once, as hw_loop relaunches the consoles (the
# hw5 bring-up arms J/H set the offsets themselves); default: the join
# after the host's radio is up.
if [ "${LAUNCH:-}" != together ]; then
  for _ in $(seq 1 240); do
    grep -q "^EVT adhoc_up" "$HL" 2>/dev/null && break
    kill -0 "$PID1" 2>/dev/null || break
    sleep 1
  done
else
  # PPSSPP only: the ad-hoc relay server runs INSIDE instance 1.  A join
  # started before it listens never connects its relay socket (EBADF,
  # "Socket error (9) when sending") and no scan ever completes -- an
  # emulator artifact with no hardware counterpart (hw5 smoke: 8/24).  So
  # the join starts once the server listens; the arms' own offsets
  # (gblink_start_frames) still model the relaunch skew.
  for _ in $(seq 1 120); do
    ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE ':(27312|37312)$' && break
    kill -0 "$PID1" 2>/dev/null || break
    sleep 0.5
  done
  ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE ':(27312|37312)$'     || echo "WARNING: no ad-hoc relay server listening on :27312/:37312"        | tee -a "$OUT/verdict.txt"
fi
PID2=$(launch 2)
for _ in $(seq 1 "$TIMEOUT_S"); do
  D1=0; D2=0
  grep -q "^EVT exit code=" "$HL" 2>/dev/null && D1=1
  grep -q "^EVT exit code=" "$JL" 2>/dev/null && D2=1
  [ "$D1" = 1 ] && [ "$D2" = 1 ] && break
  if ! kill -0 "$PID1" 2>/dev/null && ! kill -0 "$PID2" 2>/dev/null; then break; fi
  sleep 1
done
sleep 2
pkill -TERM -f "$SB/inst" 2>/dev/null || true
sleep 1; pkill -KILL -f "$SB/inst" 2>/dev/null || true
wait "$PID1" "$PID2" 2>/dev/null || true
note "wall time $(( $(date +%s) - T0 )) s"
cp "$HL" "$OUT/host.log" 2>/dev/null; cp "$JL" "$OUT/join.log" 2>/dev/null
cp "$G1/link.ap" "$OUT/host-link.ap"; cp "$G2/link.ap" "$OUT/join-link.ap"
for r in host join; do
  G="$G1"; [ "$r" = join ] && G="$G2"
  for f in "$G"/roms/*.sav; do cp "$f" "$OUT/$r-$(basename "$f")"; done
done

CRC=$(sed -n 's/.*EBOOT crc32 \([0-9a-f]*\).*/\1/p' "$RIG/MANIFEST.txt" | head -1)
ARGS=(--expect-crc "$CRC" --expect-delay 4
      --host-script "$OUT/host-link.ap" --join-script "$OUT/join-link.ap")
case "$ARM" in
  A) ARGS+=(--gen gen1 --fix-a "$RIG/golden/host-red.gb.sav" --slot-a 0
            --fix-b "$RIG/golden/join-red.gb.sav" --slot-b 0
            --host-sav "$OUT/host-red.gb.sav" --join-sav "$OUT/join-red.gb.sav"
            --expect-transfer none
            --golden-a "$RIG/reference/gen1-d4-a.sav"
            --golden-b "$RIG/reference/gen1-d4-b.sav") ;;
  C|E) ARGS+=(--expect-marks battle_start,battle_end --expect-transfer none
            --host-sav "$OUT/host-red.gb.sav" --join-sav "$OUT/join-red.gb.sav"
            --golden-a "$RIG/reference/battle-d4-a.sav"
            --golden-b "$RIG/reference/battle-d4-b.sav") ;;
  V|U) ARGS+=(--gen gen2 --fix-a "$RIG/golden/host-crystal.gbc.sav" --slot-a 0
            --fix-b "$RIG/golden/join-crystal.gbc.sav" --slot-b 2
            --host-sav "$OUT/host-crystal.gbc.sav" --join-sav "$OUT/join-crystal.gbc.sav"
            --expect-transfer none --live --expect-marks resumed,played_on
            --expect-resume live_end
            --expect-final "$(cat "$RIG/reference/live-d4.final")"
            --golden-a "$RIG/reference/live-d4-a.sav"
            --golden-b "$RIG/reference/live-d4-b.sav") ;;
  Q) ARGS+=(--gen gen2 --fix-a "$RIG/golden/host-crystal.gbc.sav" --slot-a 0
            --fix-b "$RIG/golden/join-crystal.gbc.sav" --slot-b 2
            --host-sav "$OUT/host-crystal.gbc.sav" --join-sav "$OUT/join-crystal.gbc.sav"
            --expect-transfer none --expect-marks clock_ok
            --expect-resume boot_from_battery) ;;
  R) ARGS+=(--gen gen2 --fix-a "$RIG/golden/host-crystal.gbc.sav" --slot-a 0
            --fix-b "$RIG/golden/join-crystal.gbc.sav" --slot-b 2
            --host-sav "$OUT/host-crystal.gbc.sav" --join-sav "$OUT/join-crystal.gbc.sav"
            --expect-transfer none --live --expect-marks clock_ok
            --expect-resume boot_from_battery) ;;
  B|D|L|T|P|K) ARGS+=(--gen gen2 --fix-a "$RIG/golden/host-crystal.gbc.sav" --slot-a 0
            --fix-b "$RIG/golden/join-crystal.gbc.sav" --slot-b 2
            --host-sav "$OUT/host-crystal.gbc.sav" --join-sav "$OUT/join-crystal.gbc.sav"
            --expect-transfer none
            --golden-a "$RIG/reference/gen2-d4-a.sav"
            --golden-b "$RIG/reference/gen2-d4-b.sav") ;;
  X|W) ARGS+=(--host-sav "$OUT/host-crystal.gbc.sav" --join-sav "$OUT/join-red.gb.sav"
            --expect-transfer both) ;;
  Y) ARGS+=(--host-sav "$OUT/host-synth8m.gbc.sav" --join-sav "$OUT/join-red.gb.sav"
            --expect-transfer both) ;;
  N|J|H) ARGS+=(--host-sav "$OUT/host-crystal.gbc.sav"
            --join-sav "$OUT/join-crystal.gbc.sav" --expect-transfer none) ;;
esac
python3 "$HERE/score_session.py" "$OUT/host.log" "$OUT/join.log" "${ARGS[@]}" \
  | tee "$OUT/score.txt" | tail -12 | tee -a "$OUT/verdict.txt"
exit "${PIPESTATUS[0]}"
