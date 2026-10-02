#!/usr/bin/env bash
# run_ppsspp.sh <tag> <eboot dir> <setup snippet> [timeout_s]
#
# One isolated PPSSPP run of a GBAdhoc HARNESS EBOOT for the UI overlay
# checks (docs/UI-OVERLAY.md).  The setup snippet is sourced with $GAME (the
# app dir on the emulated stick) and $FX (fixtures) set; it stages ROMs,
# states, .gpsp-harness.ini and config.ini.  log/ (GE dumps, frontend.log,
# shash.txt) and the roms/ dir land in $OUT/<tag>/.
#
# Xvfb runs on display :124 INSIDE a private user/mount/net/pid namespace
# with its own /tmp, so several runs can share the display number without
# seeing each other's lock files, and no other agent's rig is touched.
set -uo pipefail
TAG="$1"; ED="$2"; SETUP="$3"; TO="${4:-600}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/runs}"
FX="${FX:-$HOME/drprof/fx}"
SB="$HOME/gpsp-e2e/sandboxes/uiov-$TAG"
MS="$SB/ppsspp"
GAME="$MS/PSP/GAME/gpsp-adhoc"
rm -rf "$SB" "$OUT/$TAG"
mkdir -p "$MS/PSP/SYSTEM" "$GAME/log" "$GAME/roms" "$OUT/$TAG"
cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOF
[General]
FirstRun = False
CheckForNewVersion = False
EnableCheats = False

[Graphics]
GraphicsBackend = 3 (VULKAN)

[Network]
EnableWlan = False

[SystemParam]
NickName = UIOVERLAY
EOF
cp "$ED/EBOOT.PBP" "$GAME/EBOOT.PBP"
cp "$ED/gbadhoc_me.prx" "$GAME/" 2>/dev/null
cp "$FX/gba_bios.bin" "$GAME/gba_bios.bin"
export GAME FX
# shellcheck disable=SC1090
source "$SETUP"
printf '[Graphics]\nSoftwareRenderer = True\n' > "$SB/softgpu.ini"
cat > "$SB/inner.sh" <<IN
#!/usr/bin/env bash
mount -t tmpfs tmpfs /dev/shm
mount -t tmpfs tmpfs /tmp
ip link set lo up
Xvfb :124 -screen 0 960x600x24 +extension GLX +render -noreset -nolisten tcp > "$OUT/$TAG/xvfb.log" 2>&1 &
XV=\$!
sleep 1
export DISPLAY=:124
cd $HOME/ppsspp/build
XDG_CONFIG_HOME="$SB" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  timeout $TO ./PPSSPPSDL --windowed --appendconfig="$SB/softgpu.ini" "$GAME/EBOOT.PBP" > "$OUT/$TAG/emu.log" 2>&1 &
EMU=\$!
for _ in \$(seq 1 $((TO - 5))); do
  grep -q "^EVT exit" "$GAME/log/frontend.log" 2>/dev/null && break
  kill -0 \$EMU 2>/dev/null || break
  sleep 1
done
sleep 2
kill \$EMU 2>/dev/null; sleep 1; kill -9 \$EMU 2>/dev/null
kill \$XV 2>/dev/null
IN
chmod +x "$SB/inner.sh"
timeout $((TO + 30)) unshare -rmnpf --mount-proc --propagation private bash "$SB/inner.sh"
cp -r "$GAME/log/." "$OUT/$TAG/" 2>/dev/null
cp -r "$GAME/roms" "$OUT/$TAG/roms" 2>/dev/null
cp "$GAME/config.ini" "$OUT/$TAG/config.after.ini" 2>/dev/null
echo "$TAG: $(grep -m1 '^EVT exit' "$OUT/$TAG/frontend.log" 2>/dev/null) bmps=$(ls "$OUT/$TAG"/*.bmp 2>/dev/null | wc -l)"
