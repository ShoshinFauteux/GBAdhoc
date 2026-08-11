#!/usr/bin/env bash
# run_exit_room_test.sh — the exit-Union-Room case (field report 2026-08-01).
#
# WHAT HAPPENED IN THE FIELD.  Two real PSPs completed an Emerald<->Emerald
# Union Room trade over real ad-hoc radio.  When the user then walked OUT of
# the Union Room the console showed the GAME's own FATAL wireless screen
# ("...please turn off the power") — gen-3's unrecoverable RFU error path,
# not the recoverable "communication error, returning to the previous screen"
# dialog, and NOT a crash of our app: both field logs end with our orderly
# `EVT net_down` + `EVT exit code=0`, and the mirrored core payload counters
# (host core_tx=9129/core_rx=17553, client the exact reverse) say every core
# payload was delivered in both directions with overflow=0 spill=0 txfail=0.
#
# So the question this harness answers is NOT "did we crash" (we did not).
# It is: what does the emulated RFU do when the GAME ends its wireless
# session while OUR netdrv session is still up and still delivering the
# peer's in-room traffic?  Leading hypothesis: the core never tells us its
# RFU went idle, so the departing side keeps receiving RFU frames it no
# longer answers, and the side left behind sees its partner stop responding.
#
# ORACLE.  Direct: `logram cb2_*` samples gMain.callback2 (0x030022C4) at
# every interesting moment, and CB2_PrintErrorMessage|1 == 0x0800b1a1 IS the
# fatal screen (docs/AUTOPILOT.md Gate-3 table — the same predicate the
# --disconnect trade test uses to prove the host survived).  Indirect: the
# leaving script cannot reach CB2_Overworld from the fatal screen, so the
# script itself fails (ap_fail) if the game never comes back.
#
# Modes:
#   (default)         BOTH sides leave.  Host walks out first, the joiner
#                     ~11 s later (two avatars cannot share the entrance
#                     tile, and "who left first" is pinned, not raced).
#                     Both then RE-ENTER the Union Room — the re-host proof.
#   --leaver=join     Only the joiner leaves; the host stays in the room,
#                     hands off, sampling cb2.  Separates "a partner left"
#                     from "I left".
#
# Exit 0 = both games survived the room exit, returned to solo play, and
# (default mode) re-entered the Union Room.  Exit 1 = a failure, and the
# verdict names which side and which cb2 value it was sitting on.
#
# Prereqs: setup_ppsspp.sh ran once; psp/EBOOT.PBP built.  Run in WSL/Linux.
# Artifacts in tools/e2e/artifacts/exitroom[join]-<ts>/.
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
TIMEOUT_S="${TIMEOUT_S:-1500}"

EBOOT="$REPO/psp/EBOOT.PBP"
ROM="$REPO/testdata/Pokemon - Emerald Version (USA, Europe).gba"
BIOS="$REPO/testdata/gba_bios.bin"
FIX="$REPO/testdata/fixtures"

# The game's terminal wireless-error screen (gMain.callback2, Thumb bit set).
CB2_FATAL=0x0800b1a1
CB2_OVERWORLD=0x08085e5d

LEAVER=both
RADIO=""
DROPDISC=0
for arg in "$@"; do
  case "$arg" in
    --leaver=both) LEAVER=both ;;
    --leaver=join) LEAVER=join ;;
    --radio=40)    RADIO=40 ;;
    --radio=80)    RADIO=80 ;;
    --radio=160)   RADIO=160 ;;
    # CAUSATION TEST, not a normal run.  Swallows the HOST's outgoing RFU
    # DISCONNECT notices, so the joiner's emulated adapter is never told the
    # link ended and stays pinned in RFU_STATE_CLIENT.  That is the state the
    # field logs imply (host `rfu_link_down reason=4` = its own 240-frame TTL
    # expiring, joiner no breadcrumb at all).  EXPECTED RESULT IS A FAILURE:
    # the joiner should then answer SC_START (0x19, issued every ~2.3 s by
    # the Union Room's parent/child switch) with an ERROR, which gen-3 turns
    # into the UNRECOVERABLE error screen.  Run it to confirm the mechanism,
    # not to gate a build.
    --drop-disc)   DROPDISC=1 ;;
    --slow-join=*) SLOW_JOIN_US="${arg#--slow-join=}" ;;
    *) echo "usage: $0 [--leaver=both|join] [--radio=40|80|160] [--drop-disc]" >&2
       echo "                [--slow-join=US]  burn US us/frame on the JOIN side." >&2
       echo "                ADR-0047: pushing a frame past one vblank (16743 us)" >&2
       echo "                is what made the field client miss its clamp." >&2
       exit 2 ;;
  esac
done

case "$RADIO" in
  40)  LAT_MS=20; JIT_MS=10 ;;
  80)  LAT_MS=40; JIT_MS=15 ;;
  160) LAT_MS=80; JIT_MS=20 ;;
  *)   LAT_MS=0;  JIT_MS=0  ;;
esac

TAG="exitroom"
[ "$LEAVER" = join ] && TAG="exitroomjoin"
[ -n "$RADIO" ] && TAG="${TAG}r${RADIO}"
[ "$DROPDISC" = 1 ] && TAG="${TAG}-dropdisc"
ART="$SCRIPT_DIR/artifacts/$TAG-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$ART"

fail() { echo "FAIL: $*" | tee -a "$ART/verdict.txt"; exit 1; }
note() { echo "$*" | tee -a "$ART/verdict.txt"; }

[ -x "$PPSSPP_DIR/build/PPSSPPSDL" ] || fail "PPSSPPSDL not built (run setup_ppsspp.sh)"
[ -f "$EBOOT" ] || fail "psp/EBOOT.PBP missing"
[ -f "$ROM" ]   || fail "test ROM missing"
[ -f "$BIOS" ]  || fail "gba_bios.bin missing"

if [ ! -f "$FIX/emerald_parkedA.sav" ] || [ ! -f "$FIX/emerald_parkedB.sav" ]; then
  note "parked fixtures missing — generating (make_trade_fixtures.sh)"
  bash "$SCRIPT_DIR/make_trade_fixtures.sh" || fail "fixture generation failed"
fi

HOST_SCRIPT="emerald_exit_room_host.inputs"
[ "$LEAVER" = join ] && HOST_SCRIPT="emerald_exit_room_host_stay.inputs"
JOIN_SCRIPT="emerald_exit_room_join.inputs"
note "mode: leaver=$LEAVER  host script=$HOST_SCRIPT"
[ -z "$RADIO" ] || note "radio profile: ${LAT_MS}ms/side + ${JIT_MS}ms jitter"

# --- 1. Fresh 2-instance sandboxes ------------------------------------------
"$SCRIPT_DIR/setup_ppsspp.sh" --instances 2 --skip-build >/dev/null \
  || fail "setup_ppsspp.sh sandbox creation failed"

SLOW_JOIN_US=${SLOW_JOIN_US:-0}
setup_inst() { # $1=inst# $2=role $3=save $4=script $5=nick $6=rfu_drop_disconnect $7=slow_us
  local GAME="$SANDBOX_ROOT/inst$1/ppsspp/PSP/GAME/gpsp-adhoc"
  mkdir -p "$GAME/roms" "$GAME/log"
  cp "$EBOOT" "$GAME/EBOOT.PBP"
  cp "$BIOS"  "$GAME/gba_bios.bin"
  cp "$ROM"   "$GAME/roms/emerald.gba"
  cp "$3"     "$GAME/roms/emerald.sav"
  cp "$FIX/$4" "$GAME/$4"
  cat > "$GAME/.gpsp-harness.ini" <<EOF
script = $4
$2 = 1
nick = $5
net_latency_ms = $LAT_MS
net_jitter_ms = $JIT_MS
rfu_drop_disconnect = ${6:-0}
pace_slow_us = ${7:-0}
EOF
}
# Only the HOST arms the fault: it is the parent's DISCONNECT that the child
# never hears about, which is what pins the child's adapter (see --drop-disc).
setup_inst 1 host "$FIX/emerald_parkedB.sav" "$HOST_SCRIPT" host "$DROPDISC"
setup_inst 2 join "$FIX/emerald_parkedA.sav" "$JOIN_SCRIPT" join 0 "$SLOW_JOIN_US"

G1="$SANDBOX_ROOT/inst1/ppsspp/PSP/GAME/gpsp-adhoc"
G2="$SANDBOX_ROOT/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
HL="$G1/log/frontend.log"; JL="$G2/log/frontend.log"

# --- 2. Launch (inst1 first: PPSSPP_ID=1 + built-in AdhocServer) -------------
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
for _ in $(seq 1 180); do
  if grep -q "^EVT adhoc_up" "$HL" 2>/dev/null; then SRV_OK=1; break; fi
  grep -q "^EVT net_error" "$HL" 2>/dev/null && break
  kill -0 "$PID1" 2>/dev/null || break
  sleep 1
done
[ "$SRV_OK" -eq 1 ] || { cp "$HL" "$ART/host.log" 2>/dev/null; \
  fail "inst1 adhoc bring-up never completed (see host.log/inst1.emu.log)"; }
PID2=$(launch 2)

# --- 3. Wait for both to finish ---------------------------------------------
for _ in $(seq 1 "$TIMEOUT_S"); do
  D1=0; D2=0
  grep -q "^EVT exit code=" "$HL" 2>/dev/null && D1=1
  grep -q "^EVT exit code=" "$JL" 2>/dev/null && D2=1
  [ "$D1" = 1 ] && [ "$D2" = 1 ] && break
  if ! kill -0 "$PID1" 2>/dev/null && ! kill -0 "$PID2" 2>/dev/null; then break; fi
  sleep 1
done
sleep 1
# Scope the kill to THIS run's sandbox.  The old pattern matched every
# instance on the box, so two agents running harnesses concurrently shot
# each other's emulators down and both blamed their own change.
pkill -9 -f "$SANDBOX_ROOT/inst" 2>/dev/null || true
wait "$PID1" "$PID2" 2>/dev/null || true

cp "$HL" "$ART/host.log" 2>/dev/null || true
cp "$JL" "$ART/join.log" 2>/dev/null || true
mkdir -p "$ART/hostd" "$ART/joind"
cp "$G1"/log/frame_*.bmp "$ART/hostd/" 2>/dev/null || true
cp "$G2"/log/frame_*.bmp "$ART/joind/" 2>/dev/null || true
cp "$G1/roms/emerald.sav" "$ART/host.sav" 2>/dev/null || true
cp "$G2/roms/emerald.sav" "$ART/join.sav" 2>/dev/null || true
HL="$ART/host.log"; JL="$ART/join.log"
[ -f "$HL" ] || fail "host log missing"
[ -f "$JL" ] || fail "join log missing"

val() { grep "ap_val name=$2 " "$1" | tail -1 | sed -n 's/.*val=\(0x[0-9a-f]*\).*/\1/p'; }

# --- 4. The trade still has to happen (this leg is unchanged) ---------------
grep -q "ap_mark text=trade_complete" "$HL" || fail "host: trade never completed"
grep -q "ap_mark text=trade_complete" "$JL" || fail "join: trade never completed"
note "trade completed on both sides"

# --- 5. THE FATAL-SCREEN ORACLE ---------------------------------------------
# Report every cb2 sample before judging, so a failing run is self-diagnosing.
for s in host join; do
  for n in cb2_partner_left cb2_on_exit cb2_after_exit cb2_reentered \
           cb2_stay1 cb2_stay2 cb2_stay3 cb2_stay4; do
    v=$(val "$ART/$s.log" "$n")
    [ -n "$v" ] && note "$s $n = $v"
  done
done

# The adapter answering a command with an ERROR is what gen-3 turns into the
# UNRECOVERABLE variant of that screen (librfu reqResult != 0 ->
# LMAN_MSG_REQ_API_ERROR -> RFU_STATUS_FATAL_ERROR -> CB2_LinkError with
# disconnected=FALSE -> gWirelessCommType=3, which has no A-button handler:
# pokeemerald src/link.c:1589-1610,1669-1725, src/link_rfu_2.c:1995).  A
# *connection* loss yields RFU_STATUS_CONNECTION_ERROR and the recoverable
# "press A" dialog instead.  So report these FIRST — they name the cause,
# where cb2 alone cannot even tell the two screens apart.
for s in host join; do
  if grep -q "^EVT rfu_cmderr" "$ART/$s.log"; then
    note "$s: adapter answered ERROR to a command — the fatal-screen generator:"
    grep -h "^EVT rfu_cmderr" "$ART/$s.log" | sort | uniq -c \
      | sed 's/^/  /' | tee -a "$ART/verdict.txt"
  fi
  grep -q "^EVT rfu_qdrop" "$ART/$s.log" && \
    note "$s: rfu packet queue overflowed: $(grep -c '^EVT rfu_qdrop' "$ART/$s.log")"
  grep -q "^EVT rfu_unkcmd" "$ART/$s.log" && \
    note "$s: unimplemented adapter command(s): $(grep -h '^EVT rfu_unkcmd' "$ART/$s.log" | sort -u | tr '\n' ' ')"
done

FATAL=0
for s in host join; do
  if grep -q "ap_val name=cb2_[a-z0-9_]* val=$CB2_FATAL" "$ART/$s.log"; then
    note "$s: REPRODUCED — game sat on its FATAL wireless screen ($CB2_FATAL)"
    FATAL=1
  fi
done
[ "$FATAL" = 0 ] || fail "the game's fatal wireless error screen was reached"

# --- 6. Both games survive the room exit ------------------------------------
LEAVERS="host join"
[ "$LEAVER" = join ] && LEAVERS="join"
for s in $LEAVERS; do
  grep -q "ap_mark text=exit_begin" "$ART/$s.log" \
    || fail "$s: never started walking out of the Union Room"
  grep -q "ap_mark text=at_exit_tile" "$ART/$s.log" \
    || fail "$s: never reached the Union Room entrance tile (navigation)"
  grep -q "ap_mark text=left_union_room" "$ART/$s.log" \
    || fail "$s: never left the Union Room (stuck on the exit warp?)"
  L=$(val "$ART/$s.log" solo_loc)
  [ "$L" = "0x0000060a" ] || fail "$s: did not land back in Pokemon Center 2F (loc=$L)"
  grep -q "ap_mark text=back_solo" "$ART/$s.log" \
    || fail "$s: never returned to CB2_Overworld after leaving"
  note "$s: left the Union Room and returned to solo play (loc=$L)"
done

if [ "$LEAVER" = join ]; then
  grep -q "ap_mark text=ur_main_still" "$HL" \
    || fail "host: stayed in the room but its Union Room task left UR_STATE_MAIN"
  note "host: stayed in the room and held UR_STATE_MAIN while the joiner left"
fi

# --- 7. Re-host proof: back into the Union Room ------------------------------
if [ "$LEAVER" = both ]; then
  for s in host join; do
    grep -q "ap_mark text=reentered_union_room" "$ART/$s.log" \
      || fail "$s: could not re-enter the Union Room after leaving"
    grep -q "ap_mark text=ur_main2" "$ART/$s.log" \
      || fail "$s: re-entered but the Union Room task never reached UR_STATE_MAIN"
  done
  note "both sides re-entered the Union Room and reached UR_STATE_MAIN"
fi

# --- 8. Our own stack stayed healthy throughout ------------------------------
for s in host join; do
  grep -q "^EVT exit code=0" "$ART/$s.log" || fail "$s: no clean EVT exit"
  grep -q "^EVT net_error" "$ART/$s.log" && fail "$s: net_error during the run"
  LAST=$(grep "^EVT net_stats" "$ART/$s.log" | tail -1)
  [ -n "$LAST" ] || fail "$s: no net_stats"
  note "$s final: $LAST"
  echo "$LAST" | grep -Eq " overflow=0 " || fail "$s: RELIABLE payload dropped"
  echo "$LAST" | grep -Eq "drop_crc=0 drop_mal=0" || fail "$s: malformed frames"
  echo "$LAST" | grep -Eq " peers=1 " \
    || fail "$s: netdrv lost its peer (our session should outlive the room)"
  grep -q "ap_mark text=exit_room_done" "$ART/$s.log" \
    || fail "$s: script did not run to completion"
done

# The breadcrumb that tells the next field log who ended the RFU session.
if grep -q "^EVT rfu_link_down" "$HL" || grep -q "^EVT rfu_link_down" "$JL"; then
  note "rfu_link_down breadcrumbs:"
  grep -h "^EVT rfu_link_down" "$HL" "$JL" | sed 's/^/  /' | tee -a "$ART/verdict.txt"
else
  note "NOTE: no EVT rfu_link_down — the game ended its RFU session without"
  note "      taking any of rfu.c's disconnect paths (worth chasing separately)"
fi

# Frame-loop health, now that it is measurable (ADR-0019/0020).
for s in host join; do
  note "$s fps (last 3): $(grep '^EVT fps' "$ART/$s.log" | tail -3 | tr '\n' ' ')"
  note "$s sram: $(grep -c '^EVT sram_flush' "$ART/$s.log") flushes, worst \
$(grep '^EVT sram_flush' "$ART/$s.log" | sed -n 's/.* ms=\([0-9.]*\).*/\1/p' | sort -g | tail -1) ms"
done

note "PASS: both games survived the Union Room exit with no fatal wireless"
note "screen; our netdrv session outlived the game's RFU session on both sides"
exit 0
