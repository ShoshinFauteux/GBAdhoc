#!/usr/bin/env bash
# run_nettest.sh — Phase-4 bring-up step (a): PPSSPP x2 transport echo test.
# No game core involved (plan §5 Phase 4 bring-up sequence).
#
# Two sandboxed PPSSPP instances (docs/TESTING.md multi-instance recipe;
# instance 1 hosts the built-in AdhocServer) boot our EBOOT with
# .gpsp-harness.ini `nettest = 1`: the transport comes up through the full
# sceNetAdhoc bring-up (module load, sceNetInit, adhocctl connect to group
# GPSP07, PdpCreate 0x4A4B, RX thread), then the join side broadcasts 24 B
# pings at 10 Hz and the host echoes each one back to its source MAC.
# Both sides pass at >= 30 echoes and exit 0 (EVT exit code=0).
#
# Asserts (from the ms0: EVT logs): adhoc_up with distinct MACs, both
# nettest_done lines with counts >= 30, both clean EVT exits, and no
# adhocctl error events in adhoc_stats.  Exit 0 = pass.  Artifacts in
# tools/e2e/artifacts/nettest-<timestamp>/.  Run inside WSL/Linux.
#
# Prereqs: setup_ppsspp.sh ran once (PPSSPP built); psp/EBOOT.PBP built.
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
TIMEOUT_S="${TIMEOUT_S:-360}"
# Echo window: must cover the join instance's own first boot (can be
# ~60 s in a fresh sandbox) — a passing run ends early on the NTEND
# handshake, so the generous window costs nothing when things work.
NETTEST_SECS="${NETTEST_SECS:-150}"

EBOOT="$REPO/psp/EBOOT.PBP"
ART="$SCRIPT_DIR/artifacts/nettest-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }
note() { echo "$*" | tee -a "$ART/verdict.txt"; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built (run setup_ppsspp.sh)"
[ -f "$EBOOT" ] || fail "psp/EBOOT.PBP missing (build it: see psp/Makefile header)"

# --- 1. Fresh 2-instance sandboxes (TESTING.md §3-4; server on inst1) --------
"$SCRIPT_DIR/setup_ppsspp.sh" --instances 2 --skip-build >/dev/null \
  || fail "setup_ppsspp.sh sandbox creation failed"

for i in 1 2; do
  GAME="$SANDBOX_ROOT/inst$i/ppsspp/PSP/GAME/gpsp-adhoc"
  mkdir -p "$GAME/log"
  cp "$EBOOT" "$GAME/EBOOT.PBP"
  ROLE_KEY="join"; [ "$i" -eq 1 ] && ROLE_KEY="host"
  cat > "$GAME/.gpsp-harness.ini" <<EOF
nettest = 1
$ROLE_KEY = 1
nettest_secs = $NETTEST_SECS
EOF
done

# --- 2. Launch (order matters: inst1 first => PPSSPP_ID=1 + AdhocServer) -----
rm -f /dev/shm/PPSSPP_ID
LOG1="$SANDBOX_ROOT/inst1/ppsspp/PSP/GAME/gpsp-adhoc/log/frontend.log"
LOG2="$SANDBOX_ROOT/inst2/ppsspp/PSP/GAME/gpsp-adhoc/log/frontend.log"

launch() { # $1 = instance number.  NOTE: the redirection is on the SUBSHELL
  # (not the inner command) — with an inner-only redirect the backgrounded
  # subshell keeps the $(launch N) capture pipe open and the command
  # substitution blocks until the emulator exits.
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst$1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 30)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed \
      "$SANDBOX_ROOT/inst$1/ppsspp/PSP/GAME/gpsp-adhoc/EBOOT.PBP" \
  ) >"$ART/inst$1.emu.log" 2>&1 &
  echo $!
}

PID1=$(launch 1)
# Gate on inst1's own adhoc_up EVT (group registered at the built-in
# AdhocServer — strictly stronger than a bare nc probe of :27312): first
# boot in a fresh sandbox can take >60 s before the EBOOT even runs
# (Vulkan/llvmpipe warmup; see setup_ppsspp.sh boot-check comment).
SRV_OK=0
for _ in $(seq 1 180); do
  if grep -q "^EVT adhoc_up" "$LOG1" 2>/dev/null; then SRV_OK=1; break; fi
  grep -q "^EVT net_error" "$LOG1" 2>/dev/null && break
  kill -0 "$PID1" 2>/dev/null || break
  sleep 1
done
[ "$SRV_OK" -eq 1 ] || { cp "$LOG1" "$ART/host.log" 2>/dev/null; \
  fail "inst1 adhoc bring-up never completed (no adhoc_up; see host.log/inst1.emu.log)"; }
note "inst1 adhoc up; launching join instance"
PID2=$(launch 2)

# --- 3. Poll for both clean exits --------------------------------------------
for _ in $(seq 1 "$TIMEOUT_S"); do
  D1=0; D2=0
  grep -q "^EVT exit code=" "$LOG1" 2>/dev/null && D1=1
  grep -q "^EVT exit code=" "$LOG2" 2>/dev/null && D2=1
  [ "$D1" = 1 ] && [ "$D2" = 1 ] && break
  if ! kill -0 "$PID1" 2>/dev/null && ! kill -0 "$PID2" 2>/dev/null; then break; fi
  sleep 1
done
sleep 1
kill "$PID1" "$PID2" 2>/dev/null || true
wait "$PID1" "$PID2" 2>/dev/null || true

cp "$LOG1" "$ART/host.log" 2>/dev/null || true
cp "$LOG2" "$ART/join.log" 2>/dev/null || true
[ -f "$ART/host.log" ] || fail "host EVT log missing"
[ -f "$ART/join.log" ] || fail "join EVT log missing"

# --- 4. Assertions -----------------------------------------------------------
grep -q "^EVT nettest_start role=host" "$ART/host.log" || fail "host: no nettest_start"
grep -q "^EVT nettest_start role=join" "$ART/join.log" || fail "join: no nettest_start"

MAC1=$(sed -n 's/^EVT adhoc_up group=GPSP07 mac=\(.*\)$/\1/p' "$ART/host.log" | head -1)
MAC2=$(sed -n 's/^EVT adhoc_up group=GPSP07 mac=\(.*\)$/\1/p' "$ART/join.log" | head -1)
[ -n "$MAC1" ] || fail "host: adhoc bring-up failed (no adhoc_up; see net_error in host.log)"
[ -n "$MAC2" ] || fail "join: adhoc bring-up failed (no adhoc_up; see net_error in join.log)"
[ "$MAC1" != "$MAC2" ] || fail "instances share a MAC ($MAC1) — sandbox isolation broken"
note "adhoc up: host mac=$MAC1 join mac=$MAC2"

HD=$(grep "^EVT nettest_done role=host" "$ART/host.log" | tail -1)
JD=$(grep "^EVT nettest_done role=join" "$ART/join.log" | tail -1)
[ -n "$HD" ] || fail "host: no nettest_done"
[ -n "$JD" ] || fail "join: no nettest_done"
note "host: $HD"
note "join: $JD"
ECHOED=$(echo "$HD" | sed -n 's/.* echoed=\([0-9]*\).*/\1/p')
PONGS=$(echo "$JD" | sed -n 's/.* pongs=\([0-9]*\).*/\1/p')
[ "${ECHOED:-0}" -ge 30 ] || fail "host echoed only ${ECHOED:-0}/30 pings"
[ "${PONGS:-0}" -ge 30 ] || fail "join saw only ${PONGS:-0}/30 echoes"

grep -q "^EVT exit code=0" "$ART/host.log" || fail "host: no clean EVT exit"
grep -q "^EVT exit code=0" "$ART/join.log" || fail "join: no clean EVT exit"
for s in host join; do
  ST=$(grep "^EVT adhoc_stats" "$ART/$s.log" | tail -1)
  note "$s stats: $ST"
  echo "$ST" | grep -q " ctlerr=0" || fail "$s: adhocctl error events seen"
done

note "PASS: PPSSPP x2 sceNetAdhoc PDP transport echo (>=30 round trips both ways)"
exit 0
