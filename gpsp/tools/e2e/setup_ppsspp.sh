#!/usr/bin/env bash
# setup_ppsspp.sh — acquire/build PPSSPP and create N sandboxed instances.
# Spec: docs/TESTING.md ("PPSSPP multi-instance recipe"). Run inside WSL/Linux.
#
# Usage:
#   ./setup_ppsspp.sh [--instances N] [--boot-check] [--skip-build]
#
# Idempotent: safe to re-run; sandboxes are recreated from scratch each time.
set -euo pipefail

PPSSPP_REPO="https://github.com/hrydgard/ppsspp.git"
PPSSPP_COMMIT="d90fdee8fcf7f1a1d9a154cf18441233eb65c8ca"   # pinned 2026-07-30
PPSSPP_DIR="${PPSSPP_DIR:-$HOME/ppsspp}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Sandboxes MUST live on a native Linux filesystem: PPSSPP boot fails (and I/O
# crawls) when the memstick sits on a /mnt/c (DrvFs/OneDrive) mount. Verified 2026-07-31.
SANDBOX_ROOT="${SANDBOX_ROOT:-$HOME/gpsp-e2e/sandboxes}"

INSTANCES=2
BOOT_CHECK=0
SKIP_BUILD=0
while [ $# -gt 0 ]; do
  case "$1" in
    --instances) INSTANCES="$2"; shift 2 ;;
    --boot-check) BOOT_CHECK=1; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# --- 1. Host dependencies (Ubuntu package names; see TESTING.md §7-8) -------
need_pkgs=(build-essential cmake ninja-build git pkg-config
           libsdl3-dev libsdl3-ttf-dev
           xvfb libgl1-mesa-dri libglx-mesa0 netcat-openbsd)
missing=()
for p in "${need_pkgs[@]}"; do
  dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
done
if [ ${#missing[@]} -gt 0 ]; then
  echo "Missing packages: ${missing[*]}"
  echo "Run: sudo apt-get install -y ${missing[*]}"
  exit 1
fi

# --- 2. Clone + build (pinned) ----------------------------------------------
if [ "$SKIP_BUILD" -eq 0 ]; then
  if [ ! -d "$PPSSPP_DIR/.git" ]; then
    git clone --recurse-submodules --shallow-submodules "$PPSSPP_REPO" "$PPSSPP_DIR"
  fi
  git -C "$PPSSPP_DIR" fetch --depth 1 origin "$PPSSPP_COMMIT" || true
  git -C "$PPSSPP_DIR" checkout "$PPSSPP_COMMIT" 2>/dev/null || \
    echo "WARN: could not checkout pinned commit; using $(git -C "$PPSSPP_DIR" rev-parse HEAD)"
  git -C "$PPSSPP_DIR" submodule update --init --recursive --depth 1
  cmake -S "$PPSSPP_DIR" -B "$PPSSPP_DIR/build" -GNinja \
        -DCMAKE_BUILD_TYPE=Release -DUSING_QT_UI=OFF -DHEADLESS=ON >/dev/null
  ninja -C "$PPSSPP_DIR/build" PPSSPPSDL PPSSPPHeadless
fi
PPSSPP_BIN="$PPSSPP_DIR/build/PPSSPPSDL"
[ -x "$PPSSPP_BIN" ] || { echo "FATAL: $PPSSPP_BIN not built"; exit 1; }
echo "PPSSPP ready: $PPSSPP_BIN ($(git -C "$PPSSPP_DIR" rev-parse --short HEAD))"

# --- 3. Sandboxes (TESTING.md §3-4) -----------------------------------------
rm -rf "$SANDBOX_ROOT"
for i in $(seq 1 "$INSTANCES"); do
  MS="$SANDBOX_ROOT/inst$i/ppsspp"          # XDG_CONFIG_HOME=<sandbox>/instN
  mkdir -p "$MS/PSP/SYSTEM" "$MS/PSP/GAME/gpsp-adhoc/log"
  ADHOC_SRV="False"; [ "$i" -eq 1 ] && ADHOC_SRV="True"
  cat > "$MS/PSP/SYSTEM/ppsspp.ini" <<EOF
[General]
FirstRun = False
CheckForNewVersion = False

[Graphics]
; Xvfb/llvmpipe: OpenGL backend fails at first boot (no matching GLX visual for
; PPSSPP's requested config); Vulkan-on-lavapipe works. Seed it to make first
; boots deterministic instead of relying on PPSSPP's crash-and-retry fallback.
GraphicsBackend = 3 (VULKAN)

[Network]
EnableWlan = True
EnableAdhocServer = $ADHOC_SRV
proAdhocServer = localhost
AdhocServerRelayMode = 2
PortOffset = 10000
EnableUPnP = False

[SystemParam]
NickName = INST$i
MacAddress = 02:00:00:00:00:01
EOF
done
echo "Created $INSTANCES sandboxes under $SANDBOX_ROOT"

# --- 4. Optional boot check with the hello EBOOT ----------------------------
if [ "$BOOT_CHECK" -eq 1 ]; then
  HELLO="$SCRIPT_DIR/hello/EBOOT.PBP"
  [ -f "$HELLO" ] || { echo "FATAL: build tools/e2e/hello first (pspdev docker: make)"; exit 1; }
  MS="$SANDBOX_ROOT/inst1/ppsspp"
  mkdir -p "$MS/PSP/GAME/hello"
  cp "$HELLO" "$MS/PSP/GAME/hello/"
  rm -f /dev/shm/PPSSPP_ID
  LOG="$MS/PSP/GAME/hello/harness.log"
  rm -f "$LOG"
  ( cd "$PPSSPP_DIR/build" && \
    XDG_CONFIG_HOME="$SANDBOX_ROOT/inst1" SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
    timeout 180 xvfb-run -a -s "-screen 0 1280x720x24 +extension GLX +render -noreset" \
      ./PPSSPPSDL --windowed "$MS/PSP/GAME/hello/EBOOT.PBP" >"$SANDBOX_ROOT/inst1/emu.log" 2>&1 ) &
  EMU_PID=$!
  PASS=0
  for _ in $(seq 1 150); do   # poll up to 150*1s; first boot in a fresh sandbox is slow (cache warmup)
    if grep -q "EVT boot_ok" "$LOG" 2>/dev/null; then PASS=1; break; fi
    kill -0 "$EMU_PID" 2>/dev/null || break
    sleep 1
  done
  kill "$EMU_PID" 2>/dev/null || true; wait "$EMU_PID" 2>/dev/null || true
  if [ "$PASS" -eq 1 ]; then
    echo "BOOT CHECK PASS:"; cat "$LOG"
  else
    echo "BOOT CHECK FAIL: no EVT boot_ok in $LOG (emu log: $SANDBOX_ROOT/inst1/emu.log)"; exit 1
  fi
fi
echo "setup_ppsspp.sh done"
