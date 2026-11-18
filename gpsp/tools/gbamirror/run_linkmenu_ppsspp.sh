#!/usr/bin/env bash
# run_linkmenu_ppsspp.sh OUT SPEC -- one or two PPSSPP consoles driven
# through the PLAYER'S link menus (docs/GBA-LINK-UI.md): the in-game menu,
# Wireless > Host / Join / scan, the LINKING screen, the session, its end --
# by a harness `ui_script` per console, with GE dumps of every screen.  The
# game itself can be driven by an autopilot script once the link runs
# (link_script_waits = 1), as the role-driven rigs (run_session_ppsspp.sh,
# multigame/run_fixture_ppsspp.sh) drive it.
#
# SPEC is a shell file setting, per console i = 1 (and 2):
#   Ii_EB       EBOOT dir (harness/harness64 build with GBA_LINK=1)
#   Ii_ROM      ROM file;     Ii_ROMNAME  its name on the stick (default
#                             basename)
#   Ii_STATE    .st0 to autoload at frame 30 (optional)
#   Ii_SAVE     .sav (optional)
#   Ii_SCRIPT   autopilot script (optional; held until the link runs)
#   Ii_UI       ui_script file (required)
#   Ii_THEME    0 dark / 1 light (default 0)
#   Ii_MODEL    PPSSPP PSPModel (default 1 = 64 MiB)
#   Ii_INI      extra harness lines (printf %b)
#   Ii_CFG      extra config.ini lines (printf %b)
#   Ii_LINKMODE roms/linkmode.txt contents (printf %b, optional)
#   Ii_CONSOLE  console for config.ini (0 GBA, 1 GB, 2 GBC; default 0)
#   Ii_PLAIN    1 = none of the three link-rig keys (gba_link_menu,
#               link_script_waits, ui_script): a plain single-player run
#   Ii_BACKDROP a 480x272 PSP-5650 frame drawn behind the menus and the
#               LINKING screen instead of the game (linkmenu/make_backdrops.py)
#   Ii_STAGE    a shell snippet sourced last, with GAME (the app dir) and
#               ROMSTEM (roms/<ROMNAME minus its extension>) set: extra card
#               contents, e.g. the Save States page's slots (docs/UI-OVERLAY.md
#               section 13, tools/ui_model/savestates/)
# and N (1 or 2), START2 (a grep -E pattern in console 1's log that must
# appear before console 2 starts; default "^EVT adhoc_up"), TIMEOUT_S.
#
# Must run in a private namespace (fresh /dev/shm, loopback; PPSSPP_ID and
# the ad-hoc ports are per namespace):
#   unshare -rmnpf --mount-proc --propagation private bash -c \
#     'mount -t tmpfs tmpfs /dev/shm && ip link set lo up && \
#      exec bash run_linkmenu_ppsspp.sh OUT SPEC'
# Xvfb displays :143 and :144 only (XVFB_DISPLAYS overrides).  Every process
# it starts is its own process group and is killed by PID.
set -uo pipefail
OUT="$1"; SPEC="$2"
N=2; START2='^EVT adhoc_up'; TIMEOUT_S=900
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/workdirs.sh"   # specs default to $GBADHOC_WORK/...
# shellcheck disable=SC1090
. "$SPEC"
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
BIOS="${BIOS:-$HOME/gbalink/bios/gba_bios.bin}"
SB="$HOME/gpsp-e2e/sandboxes/linkmenu-$(basename "$OUT")"
read -r XD1 XD2 <<< "${XVFB_DISPLAYS:-143 144}"
rm -rf "$OUT" "$SB"; mkdir -p "$OUT"
note() { echo "$*" | tee -a "$OUT/verdict.txt"; }

v() { local n="I$1_$2"; echo "${!n:-${3:-}}"; }

setup() {  # inst
  local i=$1 MS="$SB/inst$1/ppsspp" srv=False
  local GAME="$MS/PSP/GAME/gpsp-adhoc"
  local eb rom romname st sav sc ui
  eb=$(v "$i" EB); rom=$(v "$i" ROM); romname=$(v "$i" ROMNAME "$(basename "$rom")")
  st=$(v "$i" STATE); sav=$(v "$i" SAVE); sc=$(v "$i" SCRIPT); ui=$(v "$i" UI)
  [ "$i" = 1 ] && srv=True
  mkdir -p "$MS/PSP/SYSTEM" "$GAME/roms" "$GAME/log"
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
NickName = LNKMENU$i
MacAddress = 02:00:00:00:00:0$i
PSPModel = $(v "$i" MODEL 1)
EOI
  cp "$eb/EBOOT.PBP" "$GAME/EBOOT.PBP"
  cp "$eb/gbadhoc_me.prx" "$GAME/" 2>/dev/null
  cp "$BIOS" "$GAME/gba_bios.bin"
  cp "$rom" "$GAME/roms/$romname"
  local base="${romname%.*}"
  [ -n "$st" ] && cp "$st" "$GAME/roms/$base.st0"
  [ -n "$sav" ] && cp "$sav" "$GAME/roms/$base.sav"
  [ -n "$sc" ] && cp "$sc" "$GAME/autopilot.txt"
  cp "$ui" "$GAME/ui_script.txt"
  local bd; bd=$(v "$i" BACKDROP)
  [ -n "$bd" ] && cp "$bd" "$GAME/ui_backdrop.565"
  local lm; lm=$(v "$i" LINKMODE)
  [ -n "$lm" ] && printf '%b' "$lm" > "$GAME/roms/linkmode.txt"
  {
    echo "rom = $romname"
    [ -n "$sc" ] && echo "script = autopilot.txt"
    [ -n "$st" ] && echo "load_state = 1"
    echo "heartbeat_s = 5"
    echo "me_mode = 1"
    echo "log_input = 0"
    if [ -z "$(v "$i" PLAIN)" ]; then
      echo "gba_link_menu = 1"
      echo "link_script_waits = 1"
      echo "ui_script = ui_script.txt"
    fi
    echo "nick = c$i"
    # an ini key read twice keeps the FIRST: the spec's own wins
    case "$(v "$i" INI)" in *autoexit_frames*) ;; *)
      [ -n "$(v "$i" PLAIN)" ] || echo "autoexit_frames = 60000" ;; esac
    [ -n "$bd" ] && echo "ui_backdrop = ui_backdrop.565"
    local ini; ini=$(v "$i" INI)
    [ -n "$ini" ] && printf '%b\n' "$ini"
  } > "$GAME/.gpsp-harness.ini"
  local stg; stg=$(v "$i" STAGE)
  # shellcheck disable=SC1090
  ROMSTEM="$GAME/roms/$base"
  [ -n "$stg" ] && . "$stg"
  cp "$GAME/.gpsp-harness.ini" "$OUT/c$i.ini"
  {
    printf 'theme = %s\nui_shell = 0\nscale = 1\ngroup = GPSP07\nconsole = %s\n' \
      "$(v "$i" THEME 0)" "$(v "$i" CONSOLE 0)"
    local cfg; cfg=$(v "$i" CFG)
    [ -n "$cfg" ] && printf '%b\n' "$cfg"
  } > "$GAME/config.ini"
}

for i in $(seq 1 "$N"); do setup "$i"; done
G1="$SB/inst1/ppsspp/PSP/GAME/gpsp-adhoc"; G2="$SB/inst2/ppsspp/PSP/GAME/gpsp-adhoc"
L1="$G1/log/frontend.log"; L2="$G2/log/frontend.log"
note "linkmenu N=$N eb1=$(md5sum < "$(v 1 EB)/EBOOT.PBP" | cut -c1-8) rom1=$(basename "$(v 1 ROM)") $( [ "$N" = 2 ] && echo "eb2=$(md5sum < "$(v 2 EB)/EBOOT.PBP" | cut -c1-8) rom2=$(basename "$(v 2 ROM)")")"

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
if [ "$N" = 2 ]; then
  if [ -n "$START2" ]; then
    for _ in $(seq 1 "$TIMEOUT_S"); do
      grep -qE "$START2" "$L1" 2>/dev/null && break
      kill -0 "$PID1" 2>/dev/null || break
      sleep 1
    done
  fi
  PID2=$(launch 2 "$XD2")
fi
for _ in $(seq 1 "$TIMEOUT_S"); do
  D1=0; D2=1
  grep -q "^EVT exit code=" "$L1" 2>/dev/null && D1=1
  [ "$N" = 2 ] && { D2=0; grep -q "^EVT exit code=" "$L2" 2>/dev/null && D2=1; }
  [ "$D1" = 1 ] && [ "$D2" = 1 ] && break
  if ! kill -0 "$PID1" 2>/dev/null && { [ -z "$PID2" ] || ! kill -0 "$PID2" 2>/dev/null; }; then break; fi
  sleep 1
done
sleep 2
kill -TERM -- "-$PID1" ${PID2:+"-$PID2"} 2>/dev/null || true
sleep 1; kill -KILL -- "-$PID1" ${PID2:+"-$PID2"} 2>/dev/null || true
wait "$PID1" ${PID2:+"$PID2"} 2>/dev/null || true
kill "${XP[@]}" 2>/dev/null; wait "${XP[@]}" 2>/dev/null
rm -f "/tmp/.X$XD1-lock" "/tmp/.X$XD2-lock"
note "wall time $(( $(date +%s) - T0 )) s"
for i in $(seq 1 "$N"); do
  G="$SB/inst$i/ppsspp/PSP/GAME/gpsp-adhoc"
  mkdir -p "$OUT/c$i"
  cp -r "$G/log/." "$OUT/c$i/" 2>/dev/null
  cp "$G/roms/linkmode.txt" "$OUT/c$i/linkmode.after.txt" 2>/dev/null
  cp "$G/config.ini" "$OUT/c$i/config.after.ini" 2>/dev/null
  note "c$i: $(grep -m1 '^EVT exit' "$OUT/c$i/frontend.log" 2>/dev/null) dumps=$(ls "$OUT/c$i"/*.bmp 2>/dev/null | wc -l)"
done
# SCORE=session: score_session.py on the pair (console 1 = host, 2 = join)
if [ "${SCORE:-}" = session ] && [ "$N" = 2 ]; then
  HERE="$(cd "$(dirname "$0")" && pwd)"
  cp -r "$OUT/c1" "$OUT/host"; cp -r "$OUT/c2" "$OUT/join"
  cp "$OUT/c1/frontend.log" "$OUT/host.log"; cp "$OUT/c2/frontend.log" "$OUT/join.log"
  python3 "$HERE/score_session.py" "$OUT" | tee -a "$OUT/verdict.txt"
fi
exit 0
