#!/usr/bin/env bash
# run_share_ppsspp.sh OUT SCENARIO [EB_A [EB_B]] -- "Share game data"
# (docs/SAVE-SHARE.md) end to end on two PPSSPP consoles: console 1 shares
# from its ROM browser, console 2 joins through the ordinary room list, the
# shared-data menu opens by itself, and a `ui_browser_script` per console
# presses the buttons and GE-dumps every screen.  Afterwards the files on
# both sticks are hashed and compared (verdict.txt).
#
# SCENARIO:
#   full      one state into a chosen slot + one save over an existing save
#             (-> .bak), the console switch, leave.  PASS = byte-identical
#             files on 2, the sharer's files unchanged, one .bak.
#   drop      the sharer is KILLED in the middle of a state transfer
#             (share_burst = 1 makes it slow enough).  PASS = every file on
#             2 is what it was, no temp file left.
#   mismatch  console 2 pretends to be another build (share_build): states
#             greyed with the reason, a save still fetched (no save there
#             before -> no question, no .bak).
#   full_stick  console 2's stick "fills" 50,000 bytes into a fetched file
#             (share_fail_write_after): the transfer fails cleanly, the
#             original save survives, no temp file left.
#   liar      the sharer's Slot 3 has a state's size and is not a state
#             (noise: no header of this build's format).  It is fetched,
#             its checksum is good, and it is REFUSED: err=format, every
#             guest file is what it was, no temp file left.
#   old       console 2 is an OLDER build (EB_B, e.g. a previous beta's harness build)
#             with a game open: its scan lists the share room verbatim and
#             joins it as a link room.  PASS = nobody is taken for a guest,
#             the sharer keeps sharing, the old build is not harmed.
#   ingame    console 2 is THIS build with a game open: its Settings has no
#             share rows, and its scan lists the share room dimmed and
#             refuses to join it.
#
# EB_A / EB_B: EBOOT dirs (a harness build; default $WIN/builds/save-share-out/, a local folder:
# eb-harness).  Must run in a private namespace, as every two-PPSSPP rig:
#   unshare -rmnpf --mount-proc --propagation private bash -c \
#     'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && \
#      exec bash run_share_ppsspp.sh OUT full'
# Xvfb displays :153/:154 (XVFB_DISPLAYS).  Every process it starts is its
# own process group and is stopped by PID -- never by name.
set -uo pipefail
OUT="$1"; SCEN="${2:-full}"
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../workdirs.sh"
EB_A="${3:-$GBADHOC_WORK/save-share-out/eb-harness}"; EB_B="${4:-$EB_A}"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
BIOS="${BIOS:-$HOME/gbalink/bios/gba_bios.bin}"
GBA="${GBA_ROMS:-$HOME/gbamulti/roms}"; GB="${GB_ROMS:-$HOME/gbdual/roms}"
FX="${FX:-$GBADHOC_WORK/savestate-ui-out/fx}"
HERO="${HERO:-$GBADHOC_REPO/hero}"
SB="$HOME/saveshare-agent/sb/$(basename "$OUT")"
TIMEOUT_S="${TIMEOUT_S:-420}"
read -r XD1 XD2 <<< "${XVFB_DISPLAYS:-153 154}"
rm -rf "$OUT" "$SB"; mkdir -p "$OUT"
note() { echo "$*" | tee -a "$OUT/verdict.txt"; }

EM_A="Emerald (my copy).gba"                      # same image, another name
EM_B="Pokemon - Emerald Version (USA, Europe).gba"
AW="Advance Wars 2 - Black Hole Rising (USA).gba"
GS="Golden Sun (USA, Europe).gba"
MK="Mario Kart - Super Circuit (USA).gba"
SA="Sonic Advance (USA) (En,Ja).gba"              # only the sharer has it
RED="Pokemon - Red Version (USA, Europe) (SGB Enhanced).gb"
CRY="Pokemon - Crystal Version (USA, Europe) (Rev 1).gbc"

blob() { # file size seed -- deterministic bytes
  python3 - "$1" "$2" "$3" <<'PY'
import sys, hashlib
out, n, seed = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
b = bytearray(); i = 0
while len(b) < n:
    b += hashlib.sha256(seed + i.to_bytes(4, 'little')).digest(); i += 1
open(out, 'wb').write(b[:n])
PY
}

common() { # inst script extra-ini theme
  local i=$1 MS="$SB/inst$1/ppsspp" srv=False
  local GAME="$MS/PSP/GAME/gpsp-adhoc" eb="$EB_A"
  [ "$i" = 1 ] && srv=True || eb="$EB_B"
  mkdir -p "$MS/PSP/SYSTEM" "$GAME/roms" "$GAME/log" "$GAME/hero"
  cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOI
[General]
FirstRun = False
CheckForNewVersion = False

[Graphics]
GraphicsBackend = 3 (VULKAN)
SoftwareRenderer = True

[Network]
EnableWlan = True
EnableAdhocServer = $srv
proAdhocServer = localhost
AdhocServerRelayMode = 2
PortOffset = 10000
EnableUPnP = False

[SystemParam]
NickName = SHARE$i
MacAddress = 02:00:00:00:00:0$i
PSPModel = 1
EOI
  cp "$eb/EBOOT.PBP" "$GAME/EBOOT.PBP"
  cp "$eb"/*.prx "$GAME/" 2>/dev/null
  cp "$BIOS" "$GAME/gba_bios.bin"
  cp "$2" "$GAME/script.txt"
  {
    echo "browser = 1"
    echo "ui_browser_script = script.txt"
    echo "heartbeat_s = 5"
    echo "me_mode = 1"
    echo "log_input = 0"
    echo "nick = c$i"
    [ -n "$3" ] && printf '%b\n' "$3"
  } > "$GAME/.gpsp-harness.ini"
  printf 'theme = %s\nui_shell = 1\nscale = 1\ngroup = GPSP07\nconsole = 0\nstandby = 0\n' \
    "$4" > "$GAME/config.ini"
  cp "$GAME/.gpsp-harness.ini" "$OUT/c$i.ini"; cp "$2" "$OUT/c$i.script.txt"
}

hero() { # GAME rom-name
  local base="${2%.*}"
  cp "$HERO/$base.png" "$1/hero/" 2>/dev/null
  cp "$HERO/$base.565" "$1/hero/" 2>/dev/null
}

gba_head() { # file -- give it the head of a GBA state of this build
  python3 - "$1" <<'PY'
import sys, struct, os
p = sys.argv[1]
n = os.path.getsize(p)
h = (struct.pack('<I', n) + b'\x10info-magic\0' + struct.pack('<I', 0x06BAC0DE)
     + b'\x10info-version\0' + struct.pack('<I', 0x00010004))
with open(p, 'r+b') as f:
    f.write(h)
PY
}

state() { # dir stem slot(1..5) seed size fxdir fxslot date [gba]
  local f="$1/$2.st$(( $3 - 1 ))"
  blob "$f" "$5" "$4"
  [ "${9:-}" = gba ] && gba_head "$f"
  touch -d "$8" "$f"
  if [ -n "$6" ]; then
    [ -f "$6/slot$7.thumb" ] && { cp "$6/slot$7.thumb" "$f.thumb"; touch -d "$8" "$f.thumb"; }
    [ -f "$6/slot$7.shot" ] && { cp "$6/slot$7.shot" "$f.shot"; touch -d "$8" "$f.shot"; }
  fi
  return 0
}

stage_sharer() {
  local R="$SB/inst1/ppsspp/PSP/GAME/gpsp-adhoc/roms"
  cp "$GBA/em.gba" "$R/$EM_A"; cp "$GBA/aw2.gba" "$R/$AW"
  cp "$GBA/gs1.gba" "$R/$GS";  cp "$GBA/msc.gba" "$R/$MK"
  cp "$GBA/sa1.gba" "$R/$SA";  cp "$GB/red.gb" "$R/$RED"
  # Emerald: a save and slots 1-3 with their pictures
  blob "$R/${EM_A%.*}.sav" 131072 A-em-sav; touch -d '2026-10-09 21:05' "$R/${EM_A%.*}.sav"
  # Slots 1 and 2 are states as far as the join can tell (a GBA state's
  # size and head; the rest is noise).  Slot 3 has the size and no head:
  # the file a buggy or hostile sharer would hand over (scenario liar).
  state "$R" "${EM_A%.*}" 1 A-em-1 425984 "$FX/gba" 1 '2026-09-26 19:10' gba
  state "$R" "${EM_A%.*}" 2 A-em-2 425984 "$FX/gba" 2 '2026-10-01 08:44' gba
  state "$R" "${EM_A%.*}" 3 A-em-3 425984 "$FX/gba" 3 '2026-10-03 14:32'
  # Advance Wars 2: a REAL save and state (the state can be loaded on 2)
  cp "$GBA/aw2.sav" "$R/${AW%.*}.sav"; touch -d '2026-10-07 18:00' "$R/${AW%.*}.sav"
  cp "$GBA/aw2.st0" "$R/${AW%.*}.st0"; touch -d '2026-10-07 18:02' "$R/${AW%.*}.st0"
  # Golden Sun: a save only.  Mario Kart: states only, no pictures.
  blob "$R/${GS%.*}.sav" 65536 A-gs-sav; touch -d '2026-10-02 12:00' "$R/${GS%.*}.sav"
  state "$R" "${MK%.*}" 1 A-mk-1 410000 "" 0 '2026-09-30 17:20'
  state "$R" "${MK%.*}" 4 A-mk-4 410000 "" 0 '2026-09-12 09:01'
  # Sonic Advance: console 2 does not have this game -> never listed there
  blob "$R/${SA%.*}.sav" 32768 A-sa-sav
  state "$R" "${SA%.*}" 1 A-sa-1 400000 "" 0 '2026-10-05 10:00'
  # Game Boy: Red, a save and slot 1
  blob "$R/$RED.sav" 32768 A-red-sav; touch -d '2026-10-08 20:12' "$R/$RED.sav"
  state "$R" "$RED" 1 A-red-1 50000 "$FX/gb" 1 '2026-10-08 20:12'
  # noise that must never be offered: a .bak, a temp, a stray preview
  blob "$R/${GS%.*}.sav.bak" 100 x; blob "$R/${GS%.*}.sav.gsp" 100 y
  blob "$R/${GS%.*}.st2.thumb" 5392 z
}

stage_guest() {
  local G="$SB/inst2/ppsspp/PSP/GAME/gpsp-adhoc" R
  R="$G/roms"
  cp "$GBA/em.gba" "$R/$EM_B"; cp "$GBA/aw2.gba" "$R/$AW"
  cp "$GBA/gs1.gba" "$R/$GS";  cp "$GBA/msc.gba" "$R/$MK"
  cp "$GB/red.gb" "$R/$RED";   cp "$GB/crystal.gbc" "$R/$CRY"
  for n in "$EM_B" "$AW" "$GS" "$MK" "$RED" "$CRY"; do hero "$G" "$n"; done
  # its own Emerald save, and its own states in slots 1 and 3
  blob "$R/${EM_B%.*}.sav" 131072 B-em-sav; touch -d '2026-10-07 09:30' "$R/${EM_B%.*}.sav"
  state "$R" "${EM_B%.*}" 1 B-em-1 416000 "$FX/gba" 3 '2026-09-20 23:02'
  state "$R" "${EM_B%.*}" 3 B-em-3 416000 "$FX/gba" 1 '2026-09-22 11:11'
  blob "$R/$RED.sav" 32768 B-red-sav; touch -d '2026-10-05 08:00' "$R/$RED.sav"
}

hashes() { # inst -> sorted "sha1  relative name" of everything under roms/
  ( cd "$SB/inst$1/ppsspp/PSP/GAME/gpsp-adhoc/roms" && \
    find . -type f ! -name '*.gba' ! -name '*.gb' ! -name '*.gbc' -print0 | \
    sort -z | xargs -0 sha1sum )
}

# ---- scripts ---------------------------------------------------------------
A_SCRIPT="$OUT/a.script"; B_SCRIPT="$OUT/b.script"
python3 "$HERE/mkscript.py" sharer "$SCEN" > "$A_SCRIPT"
python3 "$HERE/mkscript.py" guest "$SCEN" > "$B_SCRIPT"
A_INI=""; B_INI=""
[ -n "${BURST:-}" ] && A_INI="share_burst = $BURST"
case "$SCEN" in
  drop)       A_INI="share_burst = 1" ;;
  mismatch)   B_INI="share_build = v9.9.9-other" ;;
  full_stick) B_INI="share_fail_write_after = 50000" ;;
esac
common 1 "$A_SCRIPT" "$A_INI" 0
common 2 "$B_SCRIPT" "$B_INI" "${THEME_B:-0}"
stage_sharer
if [ "$SCEN" = ingame ]; then
  G="$SB/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
  cp "$GBA/em.gba" "$G/roms/em.gba"
  cat > "$G/ui_script.txt" <<'EOS'
+60 menu
+10 press DOWN
+4 press DOWN
+4 press DOWN
+4 press CROSS
+20 dump ig1_settings
+4 press CIRCLE
+10 press DOWN
+4 press DOWN
+4 press CROSS
+10 press DOWN
+4 press CROSS
+30 dump ig2_scan
+10 press CROSS
+20 dump ig3_refused
+4 press CIRCLE
+4 press CIRCLE
+4 press CIRCLE
+200 exit 0
EOS
  printf 'rom = em.gba\nheartbeat_s = 5\nme_mode = 1\nlog_input = 0\ngba_link_menu = 1\nlink_script_waits = 1\nui_script = ui_script.txt\nnick = c2\nautoexit_frames = 60000\n' \
    > "$G/.gpsp-harness.ini"
  cp "$G/.gpsp-harness.ini" "$OUT/c2.ini"
elif [ "$SCEN" = old ]; then
  # an older build knows nothing of the browser rows: it is driven through
  # its in-game Wireless menu (ui_script), with Emerald open
  G="$SB/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
  cp "$GBA/em.gba" "$G/roms/em.gba"
  cat > "$G/ui_script.txt" <<'EOS'
+60 menu
+10 press DOWN
+4 press DOWN
+4 press CROSS
+10 press DOWN
+4 press CROSS
+30 dump old1_scan
+10 press CROSS
+300 dump old2_joined
+900 menu
+12 dump old3_menu
+4 press CIRCLE
+300 exit 0
EOS
  printf 'rom = em.gba\nheartbeat_s = 5\nme_mode = 1\nlog_input = 0\ngba_link_menu = 1\nlink_script_waits = 1\nui_script = ui_script.txt\nnick = c2\nautoexit_frames = 60000\n' \
    > "$G/.gpsp-harness.ini"
  cp "$G/.gpsp-harness.ini" "$OUT/c2.ini"
else
  stage_guest
fi
hashes 1 > "$OUT/a.before.sha1"; hashes 2 > "$OUT/b.before.sha1"
G1="$SB/inst1/ppsspp/PSP/GAME/gpsp-adhoc"; G2="$SB/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
L1="$G1/log/frontend.log"; L2="$G2/log/frontend.log"
note "share scenario=$SCEN eb_a=$(md5sum < "$EB_A/EBOOT.PBP" | cut -c1-8) eb_b=$(md5sum < "$EB_B/EBOOT.PBP" | cut -c1-8)"

rm -f /dev/shm/PPSSPP_ID
mount -t tmpfs tmpfs /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix
XP=()
for d in $XD1 $XD2; do
  rm -f "/tmp/.X$d-lock"
  Xvfb ":$d" -screen 0 1280x720x24 +extension GLX +render -noreset -nolisten tcp \
    > "$OUT/xvfb$d.log" 2>&1 &
  XP+=($!)
done
sleep 2
launch() {  # inst display
  setsid bash -c 'cd "$1" && shift && exec "$@"' _ "$PPSSPP_DIR/build" \
    env DISPLAY=":$2" XDG_CONFIG_HOME="$SB/inst$1" SDL_AUDIODRIVER=dummy \
    LIBGL_ALWAYS_SOFTWARE=1 timeout $((TIMEOUT_S + 60)) ./PPSSPPSDL \
    --windowed "$SB/inst$1/ppsspp/PSP/GAME/gpsp-adhoc/EBOOT.PBP" \
    > "$OUT/inst$1.emu.log" 2>&1 &
  echo $!
}
T0=$(date +%s)
PID1=$(launch 1 "$XD1"); PID2=
for _ in $(seq 1 120); do
  grep -q "^EVT share_host_up" "$L1" 2>/dev/null && break
  kill -0 "$PID1" 2>/dev/null || break
  sleep 1
done
grep -q "^EVT share_host_up" "$L1" 2>/dev/null || note "FAIL: the sharer never came up"
PID2=$(launch 2 "$XD2")
KILLED=0
for _ in $(seq 1 "$TIMEOUT_S"); do
  if [ "$SCEN" = drop ] && [ "$KILLED" = 0 ] && \
     grep -q "^EVT share_xfer_start" "$L2" 2>/dev/null; then
    sleep 2                         # mid-transfer: ~120 of ~400 chunks in
    kill -KILL -- "-$PID1" 2>/dev/null
    KILLED=1
    note "sharer killed mid-transfer at $(( $(date +%s) - T0 )) s"
    ls -la "$G2/roms" > "$OUT/b.roms.at_kill.txt"
  fi
  grep -q "^EVT exit code=" "$L2" 2>/dev/null && break
  kill -0 "$PID2" 2>/dev/null || break
  sleep 1
done
# the guest is done; let the sharer's own script finish (it stops sharing
# and walks back to its browser) unless it was killed on purpose
if [ "$KILLED" = 0 ]; then
  for _ in $(seq 1 "${SHARER_TAIL_S:-150}"); do
    grep -q "^EVT exit code=" "$L1" 2>/dev/null && break
    kill -0 "$PID1" 2>/dev/null || break
    sleep 1
  done
fi
sleep 2
kill -TERM -- "-$PID1" "-$PID2" 2>/dev/null || true
sleep 1; kill -KILL -- "-$PID1" "-$PID2" 2>/dev/null || true
wait "$PID1" "$PID2" 2>/dev/null || true
kill "${XP[@]}" 2>/dev/null; wait "${XP[@]}" 2>/dev/null
rm -f "/tmp/.X$XD1-lock" "/tmp/.X$XD2-lock"
note "wall time $(( $(date +%s) - T0 )) s"
hashes 1 > "$OUT/a.after.sha1"; hashes 2 > "$OUT/b.after.sha1"
for i in 1 2; do
  G="$SB/inst$i/ppsspp/PSP/GAME/gpsp-adhoc"
  mkdir -p "$OUT/c$i"
  cp -r "$G/log/." "$OUT/c$i/" 2>/dev/null
  ls -la "$G/roms" > "$OUT/c$i.roms.txt"
  note "c$i: dumps=$(ls "$OUT/c$i"/*.bmp 2>/dev/null | wc -l) $(grep -c '^EVT share_' "$OUT/c$i/frontend.log" 2>/dev/null) share events"
done

# ---- verdict ---------------------------------------------------------------
R1="$G1/roms"; R2="$G2/roms"; FAILS=0
ok()   { note "  ok   $*"; }
bad()  { note "  FAIL $*"; FAILS=$((FAILS + 1)); }
same() { cmp -s "$1" "$2" && ok "$3" || bad "$3"; }
h()    { grep -F "  ./$2" "$OUT/$1" | cut -d' ' -f1; }
unchanged() { # b-relative-name what
  [ -n "$(h b.before.sha1 "$1")" ] && [ "$(h b.before.sha1 "$1")" = "$(h b.after.sha1 "$1")" ] \
    && ok "$2" || bad "$2"; }
cmp -s "$OUT/a.before.sha1" "$OUT/a.after.sha1" \
  && ok "the sharer's files are unchanged ($(wc -l < "$OUT/a.before.sha1") files hashed before and after)" \
  || bad "the sharer's files CHANGED"
[ -z "$(find "$R2" -name '*.gsp' -o -name '*.old')" ] \
  && ok "no temp file left on the guest" || bad "temp files left: $(find "$R2" -name '*.gsp' -o -name '*.old')"
if [ "$SCEN" = ingame ]; then
  grep -q "^EVT wl_scan_refused group=GS07DATA" "$L2" \
    && ok "with a game open the share room is listed but not joinable: $(grep '^EVT wl_scan_refused' "$L2" | cut -c5- | head -1)" \
    || bad "no refusal"
  grep -q "^EVT adhoc_join" "$L2" && bad "it joined anyway" || ok "no join was attempted"
  grep -q "^EVT share_host_guest connected=1" "$L1" && bad "a guest appeared" || ok "the sharer saw no guest"
  grep -q "^EVT exit code=0" "$L2" && ok "the game carried on" || bad "exit: $(grep '^EVT exit' "$L2" | head -1)"
  [ "$FAILS" = 0 ] && note "VERDICT PASS ($SCEN)" || note "VERDICT FAIL ($SCEN): $FAILS"
  exit 0
fi
if [ "$SCEN" = old ]; then
  grep -q "^EVT adhoc_join group=GS07DATA rc=0" "$L2" \
    && ok "the old build found the room and joined it as a link room (verbatim name)" \
    || bad "the old build did not join: $(grep '^EVT adhoc_join' "$L2" | head -1)"
  grep -q "^EVT share_host_guest connected=1" "$L1" \
    && bad "the sharer took the old build for a guest" \
    || ok "the sharer never took it for a guest"
  grep -q "^EVT share_host_stop" "$L1" && ok "the sharer kept sharing until its own player stopped it: $(grep '^EVT share_host_stop' "$L1" | cut -c5-)" \
    || bad "the sharer did not survive"
  grep -q "^EVT exit code=0" "$L2" && ok "the old build carried on and exited cleanly" \
    || bad "the old build: $(grep '^EVT exit' "$L2" | head -1)"
  grep "^EVT.*\(gbl_\|link_\|np_\|session\)" "$L2" | head -12 | sed 's/^/    /' >> "$OUT/verdict.txt"
  [ "$FAILS" = 0 ] && note "VERDICT PASS ($SCEN)" || note "VERDICT FAIL ($SCEN): $FAILS"
  exit 0
fi
grep -q "^EVT share_join_list" "$L2" && ok "the shared-data menu opened after an ordinary join" \
  || bad "the menu never opened"
grep "^EVT share_join_list" "$L2" | head -1 | grep -q "shown=4 hidden=1" \
  && ok "GBA: 4 games listed, the one the guest lacks hidden" || bad "GBA list: $(grep '^EVT share_join_list' "$L2" | head -1)"
EB="${EM_B%.*}"; EA="${EM_A%.*}"
case "$SCEN" in
  full)
    same "$R1/$EA.st1" "$R2/$EB.st2" "state: their Slot 2 == the guest's Slot 3, byte for byte"
    same "$R1/$EA.st1.thumb" "$R2/$EB.st2.thumb" "state: its thumbnail came with it"
    same "$R1/$EA.st1.shot" "$R2/$EB.st2.shot" "state: its full preview came with it"
    same "$R1/$EA.sav" "$R2/$EB.sav" "save: the guest's save == the sharer's, byte for byte"
    [ "$(sha1sum < "$R2/$EB.sav.bak" | cut -d' ' -f1)" = "$(h b.before.sha1 "$EB.sav")" ] \
      && ok "save: the guest's previous save is the .bak" || bad "save: .bak is not the previous save"
    [ "$(find "$R2" -name '*.bak' | wc -l)" = 1 ] && ok "exactly one .bak" || bad "$(find "$R2" -name '*.bak' | wc -l) .bak files"
    unchanged "$EB.st0" "the guest's other slots are untouched (Slot 1)"
    unchanged "$RED.sav" "the guest's other games are untouched (Red save)"
    grep -q "^EVT share_join_list console=1 shown=1" "$L2" && ok "console switch: GB lists Red" || bad "GB list"
    grep -q "^EVT share_join_list console=2 shown=0" "$L2" && ok "console switch: GBC is empty" || bad "GBC list"
    ;;
  drop)
    cmp -s "$OUT/b.before.sha1" "$OUT/b.after.sha1" \
      && ok "after the sharer died mid-transfer EVERY guest file is what it was" \
      || bad "guest files changed: $(diff "$OUT/b.before.sha1" "$OUT/b.after.sha1" | head -5)"
    grep -q "gsp" "$OUT/b.roms.at_kill.txt" && ok "(a temp file did exist at the moment of the kill)" \
      || note "  note no temp file seen at the kill"
    grep -q "^EVT share_xfer_fail.*err=lost" "$L2" && ok "the guest reported the loss" || bad "no loss reported"
    ;;
  mismatch)
    grep -q "^EVT share_join_open.*states_ok=0" "$L2" && ok "states are not offered across builds" || bad "states_ok"
    same "$R1/${GS%.*}.sav" "$R2/${GS%.*}.sav" "a save still crosses: Golden Sun, byte for byte"
    [ "$(find "$R2" -name '*.bak' | wc -l)" = 0 ] && ok "no save before -> no .bak" || bad ".bak without a previous save"
    unchanged "$EB.st0" "no state was touched"
    ;;
  liar)
    cmp -s "$OUT/b.before.sha1" "$OUT/b.after.sha1" \
      && ok "a file that is not a state: every guest file is what it was" \
      || bad "guest files changed: $(diff "$OUT/b.before.sha1" "$OUT/b.after.sha1" | head -5)"
    grep -q "^EVT share_xfer_fail.*err=format pct=100" "$L2" \
      && ok "the guest fetched it whole and refused it (format)" \
      || bad "no format refusal: $(grep '^EVT share_xfer_' "$L2" | tail -2)"
    grep -q "^EVT share_xfer_done" "$L2" && bad "something was committed" \
      || ok "nothing was committed"
    ;;
  full_stick)
    cmp -s "$OUT/b.before.sha1" "$OUT/b.after.sha1" \
      && ok "a full stick: every guest file is what it was" \
      || bad "guest files changed: $(diff "$OUT/b.before.sha1" "$OUT/b.after.sha1" | head -5)"
    grep -q "^EVT share_xfer_fail.*err=write" "$L2" && ok "the guest reported the write failure" || bad "no write failure"
    ;;
esac
grep "^EVT share_xfer_done" "$L2" | sed 's/^/  /' | tee -a "$OUT/verdict.txt"
[ "$FAILS" = 0 ] && note "VERDICT PASS ($SCEN)" || note "VERDICT FAIL ($SCEN): $FAILS"
exit 0
