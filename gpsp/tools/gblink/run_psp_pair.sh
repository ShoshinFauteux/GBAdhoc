#!/usr/bin/env bash
# run_psp_pair.sh -- a GB link session between TWO PPSSPP instances running
# the harness EBOOT over PPSSPP's ad-hoc (sceNetAdhoc PDP, netdrv ARQ), each
# player driven by his autopilot script.  Milestone 4.
#
#   run_psp_pair.sh OUT EBOOT_DIR ROMDIR FIXDIR GEN [options]
#     GEN          gen1 (Blue <-> Blue) | gen2 (Crystal <-> Crystal) |
#                  xfer:HOSTROM:GUESTROM (no trade: the two cartridges, each
#                  sent to the other; both play a few seconds, then the
#                  session ends -- the cartridge transfer measurement)
#     --delay=N    input delay in frames (default 4)
#     --lat=MS --jit=MS   emulated radio: each side's one-way latency and
#                  jitter (net_latency_ms / net_jitter_ms), RTT = 2 x lat
#     --xfer       the guest does NOT have the host's cartridge (it plays a
#                  copy one padding byte different), so each side's cartridge
#                  crosses the radio: 1/2 MB timings land in the log
#     --golden=DIR linkplay golden saves for this GEN and delay
#     --bulk=N     gblink_bulk: cartridge/save chunks sent per frame
#     --config=FILE  both consoles' config.ini (e.g. the owner's own CONFIG.INI
#                  from a stick backup) instead of the minimal lab one
#     --nolib      gblink_library = 0 on both: cartridges always cross
#     --loss=PCT   net_loss_pct on both: that share of received datagrams
#                  dropped (the PSP radio lost ~3 % in hardware run hw1)
#     --fat=host|join|both   emulate a PSP-1000 (PPSSPP PSPModel = 0, the
#                  32 MB console) on that side: its memory budget
#
# Must run inside a private namespace (fresh /dev/shm and loopback: the
# PPSSPP_ID and ad-hoc ports are per-namespace), e.g.
#   unshare -rmnpf --mount-proc --propagation private bash -c \
#     'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && exec bash run_psp_pair.sh ...'
#
# PASS requires, on BOTH consoles: session DONE, the save committed, every
# periodic hash matched and no desync; the two consoles' final hashes equal;
# the committed saves score as the exact trade (score_trade.py); and with
# --golden, the cartridge RAM equal to the one-process golden.  The verdict,
# logs, saves and screenshots land in OUT.
set -uo pipefail
OUT="$1"; EB="$2"; ROMS="$3"; FIX="$4"; GEN="$5"; shift 5
HERE="$(cd "$(dirname "$0")" && pwd)"
DELAY=4; LAT=0; JIT=0; XFER=0; GOLDEN=""; FAT=""; BULK=0; CONFIG=""; LIB=1; LOSS=0
for a in "$@"; do
  case "$a" in
    --delay=*) DELAY="${a#*=}" ;;
    --lat=*) LAT="${a#*=}" ;;
    --jit=*) JIT="${a#*=}" ;;
    --xfer) XFER=1 ;;
    --golden=*) GOLDEN="${a#*=}" ;;
    --fat=*) FAT="${a#*=}" ;;
    --bulk=*) BULK="${a#*=}" ;;
    --config=*) CONFIG="${a#*=}" ;;
    --nolib) LIB=0 ;;
    --loss=*) LOSS="${a#*=}" ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SB="$HOME/gpsp-e2e/sandboxes/gblink-$(basename "$OUT")"
TIMEOUT_S="${TIMEOUT_S:-1500}"
case "$GEN" in
  gen1) ROM=blue.gb; SAVE_EXT=.gb.sav; SA=0; SB_SLOT=0 ;;
  gen2) ROM=crystal.gbc; SAVE_EXT=.gbc.sav; SA=0; SB_SLOT=2 ;;
  xfer:*) XH="${GEN#xfer:}"; XG="${XH#*:}"; XH="${XH%%:*}"; ROM="$(basename "$XH")"
          SAVE_EXT=.sav ;;
  *) echo "GEN must be gen1 or gen2" >&2; exit 2 ;;
esac
rm -rf "$OUT" "$SB"
mkdir -p "$OUT"
note() { echo "$*" | tee -a "$OUT/verdict.txt"; }
fail() { note "FAIL: $*"; exit 1; }

GUEST_ROM="$ROMS/$ROM"
if [ -n "${XH:-}" ]; then
  mkdir -p "$OUT/x"
  cp "$XH" "$OUT/x/"; ROMS="$OUT/x"
  GUEST_ROM="$XG"
  printf 'wait 600
evt played
' > "$OUT/x/play.ap"
fi
if [ "$XFER" = 1 ]; then
  GUEST_ROM="$OUT/guest-$ROM"
  python3 - "$ROMS/$ROM" "$GUEST_ROM" <<'PY'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
d[-1] ^= 0x01            # unused padding at the end of the last bank
open(sys.argv[2], 'wb').write(bytes(d))
PY
fi

setup() {  # inst role rom save script
  local MS="$SB/inst$1/ppsspp" srv=False
  local GAME="$MS/PSP/GAME/gpsp-adhoc" stem model=1
  [ "$1" = 1 ] && srv=True
  case "$FAT" in both|"$2") model=0 ;; esac
  mkdir -p "$MS/PSP/SYSTEM" "$GAME/roms" "$GAME/log"
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
NickName = GBLINK$1
MacAddress = 02:00:00:00:00:0$1
PSPModel = $model
EOF
  cp "$EB/EBOOT.PBP" "$GAME/EBOOT.PBP"
  cp "$EB/gbadhoc_me.prx" "$GAME/" 2>/dev/null
  stem="$(basename "$3")"; stem="${stem%.*}"
  cp "$3" "$GAME/roms/$(basename "$3")"
  cp "$4" "$GAME/roms/$stem$SAVE_EXT"
  cp "$5" "$GAME/link.ap"
  cat > "$GAME/.gpsp-harness.ini" <<EOF
rom = $(basename "$3")
script = link.ap
$2 = 1
group = GBLINK
nick = $2
gblink_delay = $DELAY
gblink_rtc_seed = 1790500000
gblink_end_after = 120
gblink_bulk = $BULK
gblink_library = $LIB
log_input = 0
net_latency_ms = $LAT
net_jitter_ms = $JIT
net_loss_pct = $LOSS
autoexit_frames = 90000
EOF
  if [ -n "$CONFIG" ]; then tr -d '\r' < "$CONFIG" > "$GAME/config.ini"
  else printf 'theme = 0\nui_shell = 0\nscale = 1\n' > "$GAME/config.ini"; fi
}
if [ -n "${XH:-}" ]; then
  : > "$OUT/x/empty.sav"
  setup 1 host "$ROMS/$ROM" "$OUT/x/empty.sav" "$OUT/x/play.ap"
  setup 2 join "$GUEST_ROM" "$OUT/x/empty.sav" "$OUT/x/play.ap"
else
  setup 1 host "$ROMS/$ROM" "$FIX/$GEN-a.sav" "$HERE/scripts/${GEN}_host.ap"
  setup 2 join "$GUEST_ROM" "$FIX/$GEN-b.sav" "$HERE/scripts/${GEN}_guest.ap"
fi
G1="$SB/inst1/ppsspp/PSP/GAME/gpsp-adhoc"; G2="$SB/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
HL="$G1/log/frontend.log"; JL="$G2/log/frontend.log"
note "leg $GEN delay=$DELAY lat=${LAT}ms jit=${JIT}ms xfer=$XFER fat=${FAT:-none} bulk=$BULK lib=$LIB config=${CONFIG:-lab} loss=$LOSS"

rm -f /dev/shm/PPSSPP_ID
launch() {
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SB/inst$1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout $((TIMEOUT_S + 60)) xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed "$SB/inst$1/ppsspp/PSP/GAME/gpsp-adhoc/EBOOT.PBP" \
  ) > "$OUT/inst$1.emu.log" 2>&1 &
  echo $!
}
T0=$(date +%s)
PID1=$(launch 1)
for _ in $(seq 1 240); do
  grep -q "^EVT adhoc_up" "$HL" 2>/dev/null && break
  kill -0 "$PID1" 2>/dev/null || break
  sleep 1
done
grep -q "^EVT adhoc_up" "$HL" 2>/dev/null || { cp "$HL" "$OUT/host.log" 2>/dev/null; fail "host adhoc bring-up never completed"; }
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
stem="${ROM%.*}"; gstem="$(basename "$GUEST_ROM")"; gstem="${gstem%.*}"
cp "$G1/roms/$stem$SAVE_EXT" "$OUT/host.sav" 2>/dev/null
cp "$G2/roms/$gstem$SAVE_EXT" "$OUT/join.sav" 2>/dev/null
[ -f "$OUT/host.log" ] && [ -f "$OUT/join.log" ] || fail "logs missing"

rc=0
for s in host join; do
  L="$OUT/$s.log"
  note "$s: $(grep -h '^EVT exit code=' "$L" | tail -1)"
  note "$s: $(grep -h '^EVT gblink_done' "$L" | tail -1)"
  note "$s: $(grep -h '^EVT gblink_stats' "$L" | tail -1)"
  note "$s: $(grep -h '^EVT net_stats' "$L" | tail -1 | cut -c1-200)"
  grep -h 'gblink_rom_received\|gblink_rom_send' "$L" | sed "s/^/$s: /" | tee -a "$OUT/verdict.txt"
  grep -q '^EVT gblink_done' "$L" || { note "FAIL: $s session not DONE: $(grep -h 'gblink_failed\|gblink_desync\|gblink_peer_abort' "$L" | head -2)"; rc=1; }
  [ -n "${XH:-}" ] || grep -q '^EVT gblink_save_commit .* ok=1' "$L" || { note "FAIL: $s save not committed"; rc=1; }
  note "$s: $(grep -h '^EVT heap_census at=gblink' "$L" | tr '
' ' ')"
  if grep -q 'gblink_desync\|gblink_final_mismatch' "$L"; then note "FAIL: $s desync"; rc=1; fi
done
FH=$(grep -h '^EVT gblink_final' "$OUT/host.log" | tail -1 | grep -o 'hash=[0-9a-f]*')
FJ=$(grep -h '^EVT gblink_final' "$OUT/join.log" | tail -1 | grep -o 'hash=[0-9a-f]*')
[ -n "$FH" ] && [ "$FH" = "$FJ" ] && note "final hashes equal: $FH" || { note "FAIL: final hashes $FH vs $FJ"; rc=1; }
if [ -n "${XH:-}" ]; then
  note "transfer run: no trade to score"
elif [ -f "$OUT/host.sav" ] && [ -f "$OUT/join.sav" ]; then
  python3 "$HERE/score_trade.py" "$GEN" "$FIX/$GEN-a.sav" $SA "$FIX/$GEN-b.sav" $SB_SLOT \
    "$OUT/host.sav" "$OUT/join.sav" | tee -a "$OUT/verdict.txt"
  python3 "$HERE/score_trade.py" "$GEN" "$FIX/$GEN-a.sav" $SA "$FIX/$GEN-b.sav" $SB_SLOT \
    "$OUT/host.sav" "$OUT/join.sav" >/dev/null || rc=1
  if [ -n "$GOLDEN" ]; then
    if python3 - "$OUT/host.sav" "$GOLDEN-a.sav" "$OUT/join.sav" "$GOLDEN-b.sav" <<'PY'
import sys
cut = lambda d: d[:len(d) - 48] if len(d) % 256 == 48 else d
r = [open(p, 'rb').read() for p in sys.argv[1:]]
sys.exit(0 if cut(r[0]) == cut(r[1]) and cut(r[2]) == cut(r[3]) else 1)
PY
    then note "cartridge RAM equal to the one-process golden (both consoles)"
    else note "FAIL: saves differ from the golden"; rc=1; fi
  fi
else
  note "FAIL: saves missing"; rc=1
fi
[ "$rc" = 0 ] && note "VERDICT PASS" || note "VERDICT FAIL"
exit $rc
