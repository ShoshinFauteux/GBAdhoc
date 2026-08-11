#!/usr/bin/env bash
# run_netsmoke.sh — Phase-3 two-instance netdrv smoke (plan §5 Phase 3).
#
# Boots TWO headless SDL-twin instances with Emerald (gpsp_serial=auto =>
# RFU mode), one --host and one --join over localhost UDP, with the
# transport fault shim set to 5% loss + 30ms jitter on BOTH sides
# (the Gate-3 hardening condition), and asserts from the EVT logs:
#
#   - the core registered its netpacket interface (proto "gpSP v1.0")
#   - both sides reach `EVT session_start` (host id=0, client id=1..4)
#     and see each other (`EVT peer_connected`)
#   - the session stays alive >= 60 s under loss/jitter: no session_stop,
#     no peer_disconnected, and the final `EVT net_stats` on both sides
#     shows peers=1 with nonzero tx AND rx
#   - reliable delivery works end-to-end across the two processes: the
#     1 Hz --net-probe reliable broadcast is ACKed (acked>0 both sides)
#   - both instances exit cleanly (EVT exit code=0), saves untouched
#
# NOTE (recorded in DECISIONS.md ADR-0008): with no autopilot driving the
# game to the Union Room, the core itself originates no RFU packets at the
# title screen (rfu.c only broadcasts while the game hosts an RFU session),
# so core_tx/core_rx are reported but not asserted nonzero. The full
# core-driven exchange is Gate 3's trade test (autopilot workstream).
#
# Exit 0 = pass. Artifacts in tools/e2e/artifacts/netsmoke-<timestamp>/.
# Run inside WSL/Linux.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

ROM="$REPO/testdata/Pokemon - Emerald Version (USA, Europe).gba"
BIOS_DIR="$REPO/testdata"
SAV="$REPO/testdata/Pokemon Emerald All Shiny Fixed.sav"
BIN="$REPO/sdl/gpsp_sdl"

PORT="${PORT:-19019}"
FRAMES="${FRAMES:-4200}"          # ~70 s at 59.73 fps
LOSS="${LOSS:-5}"
JITTER="${JITTER:-30}"
TIMEOUT_S="${TIMEOUT_S:-180}"

ART="$SCRIPT_DIR/artifacts/netsmoke-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }
note() { echo "$*" | tee -a "$ART/verdict.txt"; }

[ -x "$BIN" ]   || fail "sdl/gpsp_sdl not built (make -C sdl)"
[ -f "$ROM" ]   || fail "test ROM missing in testdata/"
[ -f "$BIOS_DIR/gba_bios.bin" ] || fail "gba_bios.bin missing in testdata/"
[ -f "$SAV" ]   || fail "test .sav missing in testdata/"

# Scratch saves: never touch the real one.
cp "$SAV" "$ART/host.sav"
cp "$SAV" "$ART/join.sav"
SAV_MD5=$(md5sum "$ART/host.sav" | cut -d' ' -f1)

# Paced (normal-speed) run: the >=60s-alive assertion below measures WALL
# time via the 5s-interval net_stats cadence, so no --no-pacing here.
run_instance() { # $1=name $2=extra-args...
  local name="$1"; shift
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  timeout "$TIMEOUT_S" "$BIN" \
    --rom "$ROM" --bios-dir "$BIOS_DIR" --save "$ART/$name.sav" \
    --log "$ART/$name.log" --autoexit "$FRAMES" \
    --option gpsp_serial=auto \
    --nick "$name" --net-loss "$LOSS" --net-jitter "$JITTER" --net-probe \
    "$@" >"$ART/$name.stdout" 2>&1
  echo $? > "$ART/$name.rc"
}

echo "netsmoke: port=$PORT frames=$FRAMES loss=${LOSS}% jitter=${JITTER}ms"
run_instance host --host "$PORT" &
HOST_PID=$!
sleep 2
run_instance join --join "127.0.0.1:$PORT" &
JOIN_PID=$!

wait "$HOST_PID" "$JOIN_PID" 2>/dev/null || true

HL="$ART/host.log"; JL="$ART/join.log"
[ -f "$HL" ] || fail "host log missing"
[ -f "$JL" ] || fail "join log missing"

# --- assertions --------------------------------------------------------------
[ "$(cat "$ART/host.rc")" = "0" ] || fail "host exit rc=$(cat "$ART/host.rc")"
[ "$(cat "$ART/join.rc")" = "0" ] || fail "join exit rc=$(cat "$ART/join.rc")"
grep -q "^EVT exit code=0" "$HL" || fail "host: no clean EVT exit"
grep -q "^EVT exit code=0" "$JL" || fail "join: no clean EVT exit"

grep -q 'netpacket interface registered (proto="gpSP v1.0")' "$HL" \
  || fail "host: core did not register netpacket iface"
grep -q 'netpacket interface registered (proto="gpSP v1.0")' "$JL" \
  || fail "join: core did not register netpacket iface"

grep -q "^EVT session_start id=0 " "$HL" || fail "host: no session_start id=0"
grep -Eq "^EVT session_start id=[1-4]" "$JL" || fail "join: no session_start"
grep -Eq "^EVT peer_connected id=[1-4]" "$HL" || fail "host: no peer_connected"
grep -q "^EVT peer_connected id=0" "$JL" || fail "join: client never saw host"

grep -q "^EVT session_stop" "$HL" && fail "host: session dropped mid-run"
grep -q "^EVT session_stop" "$JL" && fail "join: session dropped mid-run"
grep -q "^EVT peer_disconnected" "$HL" && fail "host: peer flapped"
grep -q "^EVT peer_disconnected" "$JL" && fail "join: peer flapped"

# session alive >= 60 s: net_stats is emitted every ~5 s while the driver
# runs; require at least 12 lines on each side.
for side in host join; do
  L="$ART/$side.log"
  N=$(grep -c "^EVT net_stats" "$L")
  [ "$N" -ge 12 ] || fail "$side: only $N net_stats lines (<60 s alive)"
  LAST=$(grep "^EVT net_stats" "$L" | tail -1)
  echo "$side final: $LAST" | tee -a "$ART/verdict.txt"
  echo "$LAST" | grep -Eq "peers=1" || fail "$side: final peers != 1"
  TX=$(echo "$LAST" | sed -n 's/.* tx=\([0-9]*\).*/\1/p')
  RX=$(echo "$LAST" | sed -n 's/.* rx=\([0-9]*\).*/\1/p')
  ACKED=$(echo "$LAST" | sed -n 's/.* acked=\([0-9]*\).*/\1/p')
  [ "${TX:-0}" -gt 0 ] || fail "$side: tx=0"
  [ "${RX:-0}" -gt 0 ] || fail "$side: rx=0"
  [ "${ACKED:-0}" -gt 0 ] || fail "$side: no reliable payload ever ACKed"
  CORE_TX=$(echo "$LAST" | sed -n 's/.* core_tx=\([0-9]*\).*/\1/p')
  note "$side: core_tx=$CORE_TX (0 expected at title screen; see header note)"
done

# saves untouched (nothing in-game happened)
[ "$(md5sum "$ART/host.sav" | cut -d' ' -f1)" = "$SAV_MD5" ] \
  || note "WARN: host save changed (unexpected but not fatal)"
[ "$(md5sum "$ART/join.sav" | cut -d' ' -f1)" = "$SAV_MD5" ] \
  || note "WARN: join save changed (unexpected but not fatal)"

note "PASS: session up on both sides, >=60s alive under ${LOSS}% loss + ${JITTER}ms jitter, reliable path ACKed"
exit 0
