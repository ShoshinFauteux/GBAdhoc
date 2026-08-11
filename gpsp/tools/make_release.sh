#!/usr/bin/env bash
# make_release.sh — assemble the distributable gpSP-AdHoc / "PSP AGB" zip.
#
# This script BUILDS NOTHING.  It takes an already-built EBOOT.PBP, lays out
# the memory-stick tree a stranger can drag onto their PSP, stamps it with the
# version + commit, and zips it.  Build first (docs/RELEASE.md):
#
#   docker run --rm -v "$PWD":/build -w /build pspdev/pspdev \
#       sh -c 'make platform=psp1 clean && make platform=psp1'
#   docker run --rm -v "$PWD":/build -w /build/psp pspdev/pspdev make
#
# Usage:
#   tools/make_release.sh [EBOOT.PBP] [options]
#     EBOOT.PBP          path to the prebuilt EBOOT (default: psp/EBOOT.PBP)
#     --version X.Y.Z    release version (default: git tag, else 0.1.0)
#     --out DIR          output directory   (default: dist/)
#     --allow-stale      package an EBOOT older than the sources anyway
#                        (you must mean it; the default is to refuse)
#
# Produces:
#   <out>/psp-agb-<version>.zip      the release
#   <out>/psp-agb-<version>/         the staging tree it was zipped from
#   a manifest on stdout: every packaged file with size + sha256, then the
#   zip's own size + sha256.
#
# REFUSES to package ROMs, BIOS images, or save files.  Not a warning — the
# script exits non-zero and deletes nothing so you can see what it found.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

APPDIR_NAME="gpsp-adhoc"          # ms0:/PSP/GAME/<this>/
PRODUCT="psp-agb"

die()  { echo "FATAL: $*" >&2; exit 1; }
note() { echo "== $*"; }

# ------------------------------------------------------------------ args ---
EBOOT=""; VERSION=""; OUTDIR=""; ALLOW_STALE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --version)     VERSION="${2:-}"; shift 2 ;;
    --out)         OUTDIR="${2:-}";  shift 2 ;;
    --allow-stale) ALLOW_STALE=1;    shift ;;
    -h|--help)     sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)            die "unknown option: $1" ;;
    *)             [ -z "$EBOOT" ] || die "unexpected argument: $1"
                   EBOOT="$1"; shift ;;
  esac
done
[ -n "$EBOOT" ]  || EBOOT="$REPO/psp/EBOOT.PBP"
[ -n "$OUTDIR" ] || OUTDIR="$REPO/dist"

# ----------------------------------------------------------- the EBOOT -----
[ -f "$EBOOT" ] || die "no EBOOT at '$EBOOT'.
  Nothing is built by this script.  Build it first:
    docker run --rm -v \"\$PWD\":/build -w /build pspdev/pspdev \\
        sh -c 'make platform=psp1 clean && make platform=psp1'
    docker run --rm -v \"\$PWD\":/build -w /build/psp pspdev/pspdev make"

EBOOT_SIZE=$(wc -c < "$EBOOT" | tr -d ' ')
[ "$EBOOT_SIZE" -gt 262144 ] || die "EBOOT at '$EBOOT' is only $EBOOT_SIZE bytes.
  That is far too small to contain the gpSP core — this looks like a failed or
  partial build, not a release candidate."

# Staleness: any frontend/driver source, the psp Makefile, or the core archive
# newer than the EBOOT means you are about to ship yesterday's binary.
STALE=$(find "$REPO/psp" "$REPO/frontend-common" "$REPO/netdrv" \
             -maxdepth 1 \( -name '*.c' -o -name '*.h' -o -name 'Makefile' \) \
             -newer "$EBOOT" -print 2>/dev/null || true)
if [ -f "$REPO/gpsp_libretro_psp1.a" ] && \
   [ "$REPO/gpsp_libretro_psp1.a" -nt "$EBOOT" ]; then
  STALE="$STALE
$REPO/gpsp_libretro_psp1.a"
fi
STALE=$(echo "$STALE" | sed '/^$/d')
if [ -n "$STALE" ]; then
  if [ "$ALLOW_STALE" -eq 1 ]; then
    echo "WARNING: EBOOT is older than these sources (--allow-stale given):" >&2
    echo "$STALE" | sed 's/^/  /' >&2
  else
    echo "FATAL: '$EBOOT' is OLDER than sources that go into it:" >&2
    echo "$STALE" | sed 's/^/  /' >&2
    echo "  Rebuild the EBOOT, or pass --allow-stale if you really mean it." >&2
    exit 1
  fi
fi

# ---------------------------------------------------------- version/stamp --
if [ -z "$VERSION" ]; then
  VERSION="$(git -C "$REPO" describe --tags --abbrev=0 2>/dev/null || true)"
  VERSION="${VERSION#v}"
  [ -n "$VERSION" ] || VERSION="0.1.0"
fi
echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9]+)*$' || \
  die "version '$VERSION' does not look like X.Y.Z"

COMMIT="$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
DIRTY=""
if ! git -C "$REPO" diff --quiet HEAD -- 2>/dev/null; then DIRTY="-dirty"; fi
BUILT="$(date -u '+%Y-%m-%d %H:%M:%S UTC')"

note "packaging $PRODUCT v$VERSION (commit ${COMMIT}${DIRTY})"
note "EBOOT: $EBOOT ($EBOOT_SIZE bytes)"

# ------------------------------------------------------------- staging -----
REL="$PRODUCT-$VERSION"
STAGE="$OUTDIR/$REL"
APP="$STAGE/PSP/GAME/$APPDIR_NAME"
rm -rf "$STAGE"
mkdir -p "$APP/roms" "$APP/saves" "$APP/log"

cp "$EBOOT" "$APP/EBOOT.PBP"
# Keep the empty folders alive through zip/unzip on every extractor.
: > "$APP/roms/.keep"; : > "$APP/saves/.keep"; : > "$APP/log/.keep"

[ -f "$REPO/COPYING" ] || die "no COPYING in the repo — refusing to ship unlicensed"
cp "$REPO/COPYING" "$STAGE/LICENSE"

cat > "$STAGE/VERSION.txt" <<EOF
PSP AGB (gpSP-AdHoc)
version : $VERSION
commit  : ${COMMIT}${DIRTY}
built   : $BUILT
license : GPL-2.0-or-later (see LICENSE)
EOF
cp "$STAGE/VERSION.txt" "$APP/VERSION.txt"

cat > "$STAGE/README.txt" <<EOF
PSP AGB — gpSP-AdHoc  v$VERSION  (${COMMIT}${DIRTY})
GBA emulation for CFW PSP, with Wireless-Adapter multiplayer over PSP ad-hoc WiFi.

================================================================
READ THIS FIRST — the two things that stop wireless from working
================================================================

1) TURN THE WLAN SWITCH ON.  It is a PHYSICAL switch, and it is in a
   different place depending on your model:
       PSP-1000 (fat)   -> LEFT EDGE, below the volume controls
       PSP-2000 / 3000  -> TOP EDGE, right next to the L button
   If you own both models you will look in the wrong place on one of them.

2) PUT BOTH CONSOLES ON THE SAME FIXED AD-HOC CHANNEL.
       XMB -> Settings -> Network Settings -> Ad Hoc Mode -> Ch 1  (or 6, or 11)
   The SAME channel on both.  NOT "Automatic".
   On Automatic each PSP picks its own channel, each creates its own room with
   the same name, and they NEVER see each other — with no error message at all.

================================================================
INSTALL
================================================================

Copy the "PSP" folder in this zip to the ROOT of your memory stick.  You get:

    ms0:/PSP/GAME/$APPDIR_NAME/
        EBOOT.PBP     the app
        roms/         <- put your own .gba ROMs here
        saves/        (used by per-game builds)
        log/          frontend.log lands here

Put your legally dumped .gba ROMs in roms/.  Existing save files go NEXT TO
the ROM with the same name: Emerald.gba -> Emerald.sav
Optional: a real GBA BIOS dump as gba_bios.bin in the $APPDIR_NAME folder.
Without it the built-in open-source BIOS replacement is used, which is fine.

Do all of this on BOTH consoles, with the SAME ROM on each.
Launch: XMB -> Game -> Memory Stick -> PSP AGB

================================================================
CONTROLS
================================================================

  D-pad / O / X / L / R / Start / Select   the GBA (O = A, X = B)
  Triangle                                 cycle video scaling
  Square                                   fast-forward (hold)
  Select + Start, held ~1/4 second         in-game menu

Menu: Resume / Save state / Load state / Wireless / Settings / Exit.
X selects, O goes back.

================================================================
PLAYING WIRELESS
================================================================

  Console A: Menu -> Wireless -> Host session   (note the room code, e.g. GPSP07)
  Console B: Menu -> Wireless -> Join: scan for rooms -> pick A's room
             (or "Join room code" and D-pad it to the same code)

Then just use the game's own wireless feature — in Pokemon gen 3 that is the
Pokemon Center 2F Union Room.  Stay in the same room as each other.

Notes:
  * Fast-forward is locked to 1x while a wireless session is live (on purpose —
    the games' link timeouts are written against real time).
  * Savestates are blocked during a wireless session (the emulator core does not
    save Wireless-Adapter state, so loading one would desync your partner).
  * Your .sav is copied to .sav.bak before every wireless session starts.

Wireless quality varies by game.  Pokemon FireRed/LeafGreen/Emerald, Mario Golf
and Mega Man Battle Network work well; Digimon Racing, Shrek SuperSlam and Mario
Tennis are latency-sensitive and will feel worse than on real hardware; Hamtaro
Ham-Ham Games and Nonono Puzzle Chalien are known broken.

================================================================
CREDITS
================================================================

Wireless Adapter reverse-engineering and emulation: David Guillen Fandos
(davidgfnet) — https://github.com/davidgfnet/gpsp
Protocol documentation: Rodrigo Alfonso (afska), Corwin (blog.kuiper.dev)
Emulator core: gpSP, https://github.com/libretro/gpsp (Exophase, notaz,
davidgfnet and contributors)

GPL-2.0-or-later — see LICENSE.  No ROMs or BIOS images are included.

Full documentation: README.md in the source repository.
EOF

# ------------------------------------------- refuse to ship anyone's data --
note "checking the package for ROMs / BIOS / saves"
FORBIDDEN=$(find "$STAGE" -type f \( \
      -iname '*.gba'  -o -iname '*.gb'   -o -iname '*.gbc'  -o -iname '*.agb' \
   -o -iname '*.zip'  -o -iname '*.7z'   -o -iname '*.rar' \
   -o -iname '*.sav'  -o -iname '*.srm'  -o -iname '*.sav.bak' -o -iname '*.st[0-9]' \
   -o -iname '*bios*' -o -iname '*.bin' \) -print | sed "s|^$STAGE/||" || true)
if [ -n "$FORBIDDEN" ]; then
  echo "FATAL: refusing to build a release containing ROMs / BIOS / saves:" >&2
  echo "$FORBIDDEN" | sed 's/^/  /' >&2
  echo "  Staging tree left at: $STAGE" >&2
  exit 1
fi
# Belt and braces: nothing in the package may carry a GBA ROM header magic.
while IFS= read -r f; do
  if head -c 4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n' | grep -qi '^2e0000ea$'; then
    die "'$f' starts with a GBA ROM header — refusing to package it"
  fi
done < <(find "$STAGE" -type f ! -name 'EBOOT.PBP')
note "clean: no ROMs, BIOS images or saves in the package"

# ------------------------------------------------------------------- zip ---
ZIP="$OUTDIR/$REL.zip"
rm -f "$ZIP"
if command -v zip >/dev/null 2>&1; then
  ( cd "$OUTDIR" && zip -qr "$REL.zip" "$REL" )
elif command -v python3 >/dev/null 2>&1; then
  ( cd "$OUTDIR" && python3 -m zipfile -c "$REL.zip" "$REL" )
elif command -v powershell >/dev/null 2>&1; then
  powershell -NoProfile -Command \
    "Compress-Archive -Path '$STAGE' -DestinationPath '$ZIP' -Force" >/dev/null
else
  die "no zip, python3 or powershell available to make the archive"
fi
[ -f "$ZIP" ] || die "zip step produced no archive at $ZIP"

# -------------------------------------------------------------- manifest ---
sha256_of() {
  if   command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum    >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
  elif command -v openssl   >/dev/null 2>&1; then openssl dgst -sha256 "$1" | awk '{print $NF}'
  else echo "(no sha256 tool)"; fi
}

echo
echo "================ MANIFEST — $PRODUCT v$VERSION ================"
printf '%-12s %s\n' "version:" "$VERSION"
printf '%-12s %s\n' "commit:"  "${COMMIT}${DIRTY}"
printf '%-12s %s\n' "built:"   "$BUILT"
echo "---------------------------------------------------------------"
printf '%12s  %-64s  %s\n' "BYTES" "SHA256" "PATH"
find "$STAGE" -type f | LC_ALL=C sort | while IFS= read -r f; do
  printf '%12s  %-64s  %s\n' \
    "$(wc -c < "$f" | tr -d ' ')" "$(sha256_of "$f")" "${f#$STAGE/}"
done
echo "---------------------------------------------------------------"
printf '%12s  %-64s  %s\n' \
  "$(wc -c < "$ZIP" | tr -d ' ')" "$(sha256_of "$ZIP")" "$(basename "$ZIP")"
echo "==============================================================="
echo
note "release  : $ZIP"
note "staging  : $STAGE"
note "next     : docs/RELEASE.md (tag, attach, upstream courtesy step)"
