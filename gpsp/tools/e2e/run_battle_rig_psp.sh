#!/usr/bin/env bash
# run_battle_rig_psp.sh -- the double-battle rig (docs/RIG-DOUBLE-BATTLE.md) on
# TWO PPSSPP instances: the G-phase bring-up and validation runner.  Same EBOOT,
# same fixtures, same harness keys and same scoring as the hardware loop; only
# the transport under the radio differs (PPSSPP loopback + our fault shim).
#
#   HOST_SAV=... JOIN_SAV=... ./run_battle_rig_psp.sh [--radio=40|80] [--arm=A|B]
#
# Env:
#   HOST_SAV / JOIN_SAV   rig golden saves (make_rig_golden.py output)
#   HOST_ROM / JOIN_ROM   default FireRed / LeafGreen rev 1 from testdata/
#   HOST_SCRIPT / JOIN_SCRIPT   default frlg_battle_{host,join}.inputs
#   HOST_EXTRA_INI / JOIN_EXTRA_INI   extra harness lines (validation arms)
#   RUN_ID                default <arm>-<timestamp>
#   TIMEOUT_S             default 2400
#   TURNS                 override the fixture's 30-turn cap
# Exit 0 when both consoles exited; SCORING is summarize_battle.py's job.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/adhoc_lock.sh"; adhoc_lock
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"
TIMEOUT_S="${TIMEOUT_S:-2400}"
EBOOT="$REPO/psp/EBOOT.PBP"
BIOS="$REPO/testdata/gba_bios.bin"
FIX="$REPO/testdata/fixtures"
HOST_ROM="${HOST_ROM:-$REPO/testdata/Pokemon - FireRed Version (USA, Europe) (Rev 1).gba}"
JOIN_ROM="${JOIN_ROM:-$REPO/testdata/Pokemon - LeafGreen Version (USA, Europe) (Rev 1).gba}"
HOST_SCRIPT="${HOST_SCRIPT:-frlg_battle_host.inputs}"
JOIN_SCRIPT="${JOIN_SCRIPT:-frlg_battle_join.inputs}"
RADIO=""; ARM="B"
for arg in "$@"; do
  case "$arg" in
    --radio=*) RADIO="${arg#--radio=}" ;;
    --arm=*)   ARM="${arg#--arm=}" ;;
    *) echo "usage: $0 [--radio=40|80] [--arm=A|B]" >&2; exit 2 ;;
  esac
done
case "$RADIO" in 40) LAT=20; JIT=10 ;; 80) LAT=40; JIT=15 ;; *) LAT=0; JIT=0 ;; esac
case "$ARM" in A) KEEP=0; HOLD=0 ;; B) KEEP=2; HOLD=1 ;; *) echo "arm must be A or B" >&2; exit 2 ;; esac
RUN_ID="${RUN_ID:-$ARM-$(date +%s)}"
ART="$SCRIPT_DIR/artifacts/battle-$ARM${RADIO:+-r$RADIO}-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"
fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }
for f in "$EBOOT" "$BIOS" "$HOST_ROM" "$JOIN_ROM" "${HOST_SAV:?}" "${JOIN_SAV:?}" \
         "$FIX/$HOST_SCRIPT" "$FIX/$JOIN_SCRIPT"; do
  [ -f "$f" ] || fail "missing $f"
done

"$SCRIPT_DIR/setup_ppsspp.sh" --instances 2 --skip-build >/dev/null || fail "sandbox setup"
FAIL_PROBE="0x030030F4:4,0x02023E8A:1,0x03005AEE:1,0x0203ADE8:1,0x0203ADE6:1,0x03004FE0:4,0x03000FA4:4,0x0202004F:2"

setup_inst() { # $1 inst $2 role $3 rom $4 sav $5 script $6 extra
  local G="$SANDBOX_ROOT/inst$1/ppsspp/PSP/GAME/gpsp-adhoc"
  mkdir -p "$G/roms" "$G/log"
  cp "$EBOOT" "$G/EBOOT.PBP"
  [ -f "$REPO/psp/me/gbadhoc_me.prx" ] && cp "$REPO/psp/me/gbadhoc_me.prx" "$G/"
  cp "$BIOS" "$G/gba_bios.bin"
  cp "$3" "$G/roms/rig.gba"
  cp "$4" "$G/roms/rig.sav"
  cp "$FIX/$5" "$G/$5"
  # TURNS=N rewrites the turn cap in the STAGED copy (the realistic-traffic
  # variant runs fewer, longer turns); the log's ap_loaded crc then names the
  # exact script that ran.
  [ -n "${TURNS:-}" ] && sed -i "s/^repeat 30$/repeat $TURNS/" "$G/$5"
  cp "$G/$5" "$ART/$2.inputs"
  cat > "$G/.gpsp-harness.ini" <<EOF
script = $5
$2 = 1
nick = $2
run_id = $RUN_ID
arm = $ARM
rfu_shed_keep = $KEEP
rfu_hold = $HOLD
log_input = 0
fail_probe = $FAIL_PROBE
watch_ram = 0x02036E6C:4,0x02036E90:4
net_latency_ms = $LAT
net_jitter_ms = $JIT
EOF
  # An extra line REPLACES the base line for its key: fe_ini_get returns the
  # FIRST match, so an appended duplicate would be silently ignored (it was:
  # the first V1 latency run measured nothing).  The build now also reports
  # duplicates as cfg_duplicate and the scorer calls such a run INVALID.
  if [ -n "$6" ]; then
    printf '%s\n' "$6" | while IFS= read -r l; do
      k=$(printf '%s' "$l" | sed -n 's/^ *\([a-z_0-9]*\) *=.*/\1/p')
      [ -n "$k" ] && sed -i "/^$k *=/d" "$G/.gpsp-harness.ini"
    done
    printf '%s\n' "$6" >> "$G/.gpsp-harness.ini"
  fi
  cp "$G/.gpsp-harness.ini" "$ART/$2.ini"
  # HOST_CONFIG / JOIN_CONFIG: stage a real CONFIG.INI (e.g. the owner's, or a
  # rig stage's) as this instance's config.ini, the way hw_loop stages it.
  local cv; cv=$(echo "$2" | tr a-z A-Z)_CONFIG
  if [ -n "${!cv:-}" ]; then cp "${!cv}" "$G/config.ini"; cp "${!cv}" "$ART/$2-CONFIG.INI"; fi
}
setup_inst 1 host "$HOST_ROM" "$HOST_SAV" "$HOST_SCRIPT" "${HOST_EXTRA_INI:-}"
setup_inst 2 join "$JOIN_ROM" "$JOIN_SAV" "$JOIN_SCRIPT" "${JOIN_EXTRA_INI:-}"
md5sum "$EBOOT" "$HOST_SAV" "$JOIN_SAV" "$FIX/$HOST_SCRIPT" "$FIX/$JOIN_SCRIPT" > "$ART/manifest.md5"

G1="$SANDBOX_ROOT/inst1/ppsspp/PSP/GAME/gpsp-adhoc"
G2="$SANDBOX_ROOT/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
HL="$G1/log/frontend.log"; JL="$G2/log/frontend.log"
# The AdhocServer port is machine-global: a foreign PPSSPP (another agent's
# solo sandbox with the server enabled) makes our adhocctl connect fail BUSY.
# Wait for it rather than produce a run that looks like a transport failure.
for _ in $(seq 1 180); do
  ss -ltnp 2>/dev/null | grep -q ':27312 ' || break
  [ "${_}" = 1 ] && echo "waiting: port 27312 held by $(ss -ltnp | grep ':27312 ' | grep -o 'pid=[0-9]*')" | tee -a "$ART/verdict.txt"
  sleep 10
done
ss -ltnp 2>/dev/null | grep -q ':27312 ' && fail "AdhocServer port 27312 still held by a foreign process"
rm -f /dev/shm/PPSSPP_ID
# xvfb-run's Xvfb outlives a killed emulator (reparented to init) and 35 of
# them had accumulated here.  Reap exactly OURS: the Xvfb processes descended
# from the two launches, recorded while their parents are still alive.  (The
# first version reaped every new 1280x720 Xvfb, which would also kill another
# agent's display that happened to start during our run.)
OURX=""
is_ours() { # $1 pid: true if PID1 or PID2 is an ancestor
  local p=$1 n=0
  while [ -n "$p" ] && [ "$p" -gt 1 ] && [ $n -lt 12 ]; do
    [ "$p" = "${PID1:-x}" ] || [ "$p" = "${PID2:-x}" ] && return 0
    p=$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' '); n=$((n + 1))
  done
  return 1
}
note_ours() {
  local x
  for x in $(pgrep -x Xvfb); do
    case " $OURX " in *" $x "*) ;; *) is_ours "$x" && OURX="$OURX $x" ;; esac
  done
}
launch() {
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst$1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 60)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed "$SANDBOX_ROOT/inst$1/ppsspp/PSP/GAME/gpsp-adhoc/EBOOT.PBP" \
  ) >"$ART/inst$1.emu.log" 2>&1 &
  echo $!
}
PID1=$(launch 1)
for _ in $(seq 1 180); do
  grep -q "^EVT adhoc_up" "$HL" 2>/dev/null && break
  kill -0 "$PID1" 2>/dev/null || break
  sleep 1
done
PID2=$(launch 2)
sleep 3; note_ours
# LIVENESS IS NOT LOG GROWTH: wait for both EVT exit lines or the processes,
# bounded only by TIMEOUT_S.  A starved log writer must not read as a stall.
FIRST_EXIT=0
for i in $(seq 1 "$TIMEOUT_S"); do
  D1=0; D2=0
  grep -q "^EVT exit code=" "$HL" 2>/dev/null && D1=1
  grep -q "^EVT exit code=" "$JL" 2>/dev/null && D2=1
  [ "$D1" = 1 ] && [ "$D2" = 1 ] && break
  [ $((i % 30)) = 0 ] && note_ours
  if ! kill -0 "$PID1" 2>/dev/null && ! kill -0 "$PID2" 2>/dev/null; then break; fi
  # One side finished and the other has had 5 minutes to follow: the pair is
  # over (a live peer's own link timeout is seconds).  Keep what it logged.
  if [ $((D1 + D2)) = 1 ]; then
    [ "$FIRST_EXIT" = 0 ] && FIRST_EXIT=$i
    [ $((i - FIRST_EXIT)) -gt 300 ] && { echo "peer did not exit 300 s after the other" >> "$ART/verdict.txt"; break; }
  fi
  sleep 1
done
sleep 2
note_ours
pkill -9 -f "$SANDBOX_ROOT/inst" 2>/dev/null || true
wait "$PID1" "$PID2" 2>/dev/null || true
for x in $OURX; do kill "$x" 2>/dev/null; done
rm -f /dev/shm/PPSSPP_ID
cp "$HL" "$ART/host.log" 2>/dev/null; cp "$JL" "$ART/join.log" 2>/dev/null
for g in "$G1" "$G2"; do
  r=host; [ "$g" = "$G2" ] && r=join
  mkdir -p "$ART/${r}d"; cp "$g"/log/frame_*.bmp "$ART/${r}d/" 2>/dev/null || true
  cp "$g/log/frontend.prev.log" "$ART/$r.prev.log" 2>/dev/null || true
done
echo "$ART"
