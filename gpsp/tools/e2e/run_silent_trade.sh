#!/usr/bin/env bash
# run_silent_trade.sh — silent-wireless policy e2e (ADR-0013, VARIANTS.md).
#
# Same rig, fixtures, scripts and oracle as run_trade_test_psp.sh, but the
# session is NOT pre-established from autopilot host=1/join=1 keys.
# Instead both instances run with the variant silent-wireless policy
# (autopilot silent=1 exercises it on the generic build):
#
#   inst1: silent=1, role auto  — its game activates the RFU adapter at
#          the Union Room attendant (weak core hook fires), the frontend
#          joins, finds nobody within the jittered window, and PROMOTES
#          itself to host (EVT silent_host promoted=1).
#   inst2: silent=1, role=join  — activates later (launch-gated on inst1's
#          promotion), its JOIN latches onto inst1 (EVT silent_joined).
#
# Then the ordinary autonomous Union Room board trade must complete with
# the full Gate-4E oracle: personalities swapped in RAM and in the saved
# .sav on both sides, driver health clean, clean exits.
#
# Exit 0 = pass.  Artifacts in tools/e2e/artifacts/silenttrade-<ts>/.
# Run inside WSL/Linux.  Prereqs: setup_ppsspp.sh ran once; psp/EBOOT.PBP.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Serialise against every other ad-hoc test on this machine.  PPSSPP's
# AdhocServer port and /dev/shm/PPSSPP_ID are global, so SANDBOX_ROOT does
# NOT make two concurrent runs independent -- they corrupt each other and
# the symptom looks like a transport regression.  See adhoc_lock.sh.
. "$SCRIPT_DIR/adhoc_lock.sh"; adhoc_lock
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"
TIMEOUT_S="${TIMEOUT_S:-900}"

EBOOT="$REPO/psp/EBOOT.PBP"
ROM="$REPO/testdata/Pokemon - Emerald Version (USA, Europe).gba"
BIOS="$REPO/testdata/gba_bios.bin"
FIX="$REPO/testdata/fixtures"

ART="$SCRIPT_DIR/artifacts/silenttrade-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }
note() { echo "$*" | tee -a "$ART/verdict.txt"; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built"
[ -f "$EBOOT" ] || fail "psp/EBOOT.PBP missing"
[ -f "$ROM" ]   || fail "test ROM missing"
[ -f "$BIOS" ]  || fail "gba_bios.bin missing"

if [ ! -f "$FIX/emerald_parkedA.sav" ] || [ ! -f "$FIX/emerald_parkedB.sav" ]; then
  note "parked fixtures missing — generating (make_trade_fixtures.sh)"
  bash "$SCRIPT_DIR/make_trade_fixtures.sh" || fail "fixture generation failed"
fi

# Pre-trade identities (oracle inputs).
python3 "$SCRIPT_DIR/read_party.py" "$FIX/emerald_parkedB.sav" >"$ART/pre_host.txt" \
  || fail "cannot decode parkedB"
python3 "$SCRIPT_DIR/read_party.py" "$FIX/emerald_parkedA.sav" >"$ART/pre_join.txt" \
  || fail "cannot decode parkedA"
A0_PERS=$(awk '/^mon slot=0 /{print $3}' "$ART/pre_join.txt" | cut -d= -f2)
A0_SPEC=$(awk '/^mon slot=0 /{print $5}' "$ART/pre_join.txt" | cut -d= -f2)
B1_PERS=$(awk '/^mon slot=1 /{print $3}' "$ART/pre_host.txt" | cut -d= -f2)
B1_SPEC=$(awk '/^mon slot=1 /{print $5}' "$ART/pre_host.txt" | cut -d= -f2)
[ -n "$A0_PERS" ] && [ -n "$B1_PERS" ] || fail "pre-trade party decode failed"

# --- 1. Fresh 2-instance sandboxes -------------------------------------------
"$SCRIPT_DIR/setup_ppsspp.sh" --instances 2 --skip-build >/dev/null \
  || fail "setup_ppsspp.sh sandbox creation failed"

setup_inst() { # $1=inst# $2=role-line $3=save $4=script $5=nick
  local GAME="$SANDBOX_ROOT/inst$1/ppsspp/PSP/GAME/gpsp-adhoc"
  mkdir -p "$GAME/roms" "$GAME/log"
  cp "$EBOOT" "$GAME/EBOOT.PBP"
  cp "$BIOS"  "$GAME/gba_bios.bin"
  cp "$ROM"   "$GAME/roms/emerald.gba"
  cp "$3"     "$GAME/roms/emerald.sav"
  cp "$FIX/$4" "$GAME/$4"
  cat > "$GAME/.gpsp-harness.ini" <<EOF
script = $4
silent = 1
$2
nick = $5
EOF
}
setup_inst 1 ""          "$FIX/emerald_parkedB.sav" "emerald_trade_host.inputs" shost
setup_inst 2 "role = join" "$FIX/emerald_parkedA.sav" "emerald_trade_join.inputs" sjoin

G1="$SANDBOX_ROOT/inst1/ppsspp/PSP/GAME/gpsp-adhoc"
G2="$SANDBOX_ROOT/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
HL="$G1/log/frontend.log"; JL="$G2/log/frontend.log"

# --- 2. Launch: inst1 first; gate inst2 on inst1's silent promotion ----------
rm -f /dev/shm/PPSSPP_ID
launch() {
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst$1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 60)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed \
      "$SANDBOX_ROOT/inst$1/ppsspp/PSP/GAME/gpsp-adhoc/EBOOT.PBP" \
  ) >"$ART/inst$1.emu.log" 2>&1 &
  echo $!
}
PID1=$(launch 1)
SRV_OK=0
for _ in $(seq 1 240); do
  if grep -q "^EVT silent_host" "$HL" 2>/dev/null; then SRV_OK=1; break; fi
  grep -q "^EVT silent_fail" "$HL" 2>/dev/null && break
  kill -0 "$PID1" 2>/dev/null || break
  sleep 1
done
[ "$SRV_OK" -eq 1 ] || { cp "$HL" "$ART/host.log" 2>/dev/null; \
  fail "inst1 never silently promoted to host (see host.log)"; }
note "inst1 silently promoted: $(grep '^EVT silent_host' "$HL" | head -1)"
PID2=$(launch 2)

# --- 3. Wait for both to finish ----------------------------------------------
for _ in $(seq 1 "$TIMEOUT_S"); do
  D1=0; D2=0
  grep -q "^EVT exit code=" "$HL" 2>/dev/null && D1=1
  grep -q "^EVT exit code=" "$JL" 2>/dev/null && D2=1
  [ "$D1" = 1 ] && [ "$D2" = 1 ] && break
  if ! kill -0 "$PID1" 2>/dev/null && ! kill -0 "$PID2" 2>/dev/null; then break; fi
  sleep 1
done
sleep 1
pkill -9 -f "$SANDBOX_ROOT/inst" 2>/dev/null || true
wait "$PID1" "$PID2" 2>/dev/null || true

cp "$HL" "$ART/host.log" 2>/dev/null || true
cp "$JL" "$ART/join.log" 2>/dev/null || true
cp "$G1/roms/emerald.sav" "$ART/host.sav" 2>/dev/null || true
cp "$G2/roms/emerald.sav" "$ART/join.sav" 2>/dev/null || true
HL="$ART/host.log"; JL="$ART/join.log"
[ -f "$HL" ] && [ -f "$JL" ] || fail "logs missing"

# --- 4. Silent-policy assertions ---------------------------------------------
grep -q "^EVT silent_activate role=join" "$HL" || fail "host: no auto join-first activation"
grep -q "^EVT silent_host promoted=1" "$HL"    || fail "host: never promoted"
grep -q "^EVT silent_activate role=join" "$JL" || fail "join: no pinned-join activation"
grep -q "^EVT silent_joined" "$JL"             || fail "join: never latched onto silent host"
grep -q "^EVT sav_backup" "$HL" || fail "host: pre-session save backup missing"
grep -q "^EVT sav_backup" "$JL" || fail "join: pre-session save backup missing"

# --- 5. Session + trade oracle (same as run_trade_test_psp.sh) ---------------
grep -q "^EVT session_start id=0 " "$HL" || fail "host: no session_start id=0"
grep -Eq "^EVT session_start id=[1-4]" "$JL" || fail "join: no session_start"
grep -Eq "^EVT peer_connected id=[1-4]" "$HL" || fail "host: no peer_connected"
grep -q "^EVT peer_connected id=0" "$JL" || fail "join: never saw host"
for s in host join; do
  grep -q "ap_mark text=in_union_room" "$ART/$s.log" || fail "$s: never entered Union Room"
  grep -q "ap_mark text=trade_anim" "$ART/$s.log" || fail "$s: trade never started"
  grep -q "ap_mark text=trade_complete" "$ART/$s.log" || fail "$s: no trade_complete"
  grep -q "ap_done" "$ART/$s.log" || fail "$s: script did not finish"
  grep -q "^EVT exit code=0" "$ART/$s.log" || fail "$s: no clean exit"
  LAST=$(grep "^EVT net_stats" "$ART/$s.log" | tail -1)
  [ -n "$LAST" ] || fail "$s: no net_stats"
  note "$s final: $LAST"
  echo "$LAST" | grep -Eq " overflow=0 " || fail "$s: reliable payloads dropped"
  echo "$LAST" | grep -Eq "drop_crc=0 drop_mal=0" || fail "$s: malformed frames"
  ACKED=$(echo "$LAST" | sed -n 's/.* acked=\([0-9]*\).*/\1/p')
  [ "${ACKED:-0}" -gt 0 ] || fail "$s: nothing ever ACKed"
done

val() { grep "ap_val name=$2 " "$1" | tail -1 | sed -n 's/.*val=\(0x[0-9a-f]*\).*/\1/p'; }
H_PRE1=$(val "$HL" pre1); H_POST1=$(val "$HL" post1)
J_PRE0=$(val "$JL" pre0); J_POST0=$(val "$JL" post0)
[ "$H_POST1" = "$J_PRE0" ] || fail "oracle: host slot2 != join's offered mon"
[ "$J_POST0" = "$H_PRE1" ] || fail "oracle: join slot1 != host's registered mon"
[ "$H_POST1" != "$H_PRE1" ] || fail "oracle: host slot2 unchanged"

python3 "$SCRIPT_DIR/read_party.py" "$ART/host.sav" >"$ART/post_host.txt" \
  || fail "post host save unreadable"
python3 "$SCRIPT_DIR/read_party.py" "$ART/join.sav" >"$ART/post_join.txt" \
  || fail "post join save unreadable"
grep -q "slot=1 personality=$A0_PERS .*species=$A0_SPEC" "$ART/post_host.txt" \
  || fail "host .sav: received mon not saved"
grep -q "slot=0 personality=$B1_PERS .*species=$B1_SPEC" "$ART/post_join.txt" \
  || fail "join .sav: received mon not saved"

note "PASS: silent-wireless policy negotiated the session with zero UI"
note "(auto promote-to-host on inst1, join latch on inst2) and the full"
note "Union Room trade + oracle held end-to-end"
exit 0
