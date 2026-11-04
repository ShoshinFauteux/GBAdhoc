#!/usr/bin/env bash
# test_xmbwrap.sh -- xmbwrap's host test (WSL/Linux, docker image pspdev/pspdev).
# A fake card (dummy ROM bytes, no real game), one entry per case, checked by
# check_entry.py: PBP structure, DATA.PSP == the launcher's, PARAM.SFO
# byte-identical to the SDK's own mksfoex for the same title, art sizes,
# launch.ini.  Needs psp/launcher/EBOOT.PBP built (psp/launcher/Makefile).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../.." && pwd)"
TPL="$TREE/psp/launcher/EBOOT.PBP"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
[ -f "$TPL" ] || { echo "build psp/launcher first"; exit 2; }
gcc -O2 -Wall -Wextra -o "$W/xmbwrap" "$HERE/xw_cli.c" "$HERE/xw_core.c" -lm || exit 2

CARD="$W/card"; GB="$CARD/PSP/GAME/GBAdhoc"
mkdir -p "$GB/roms/sub" "$GB/hero" "$GB/boxart"
head -c 4096 /dev/urandom > "$GB/roms/Test Game (USA).gba"
head -c 4096 /dev/urandom > "$GB/roms/sub/Other.gbc"
python3 - "$GB" <<'EOF'
import sys, struct, zlib
def png(path, w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    def ch(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           ch(b"IDAT", zlib.compress(raw)) + ch(b"IEND", b""))
def bmp(path, w, h, bgr):
    row = (bytes(bgr) * w + b"\0" * ((4 - w * 3 % 4) % 4))
    data = row * h
    open(path, "wb").write(b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54) +
                           struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0) + data)
g = sys.argv[1]
png(g + "/hero/Test Game (USA).png", 480, 272, (10, 120, 200))
bmp(g + "/boxart/Test Game (USA).bmp", 112, 112, (0, 200, 50))
png(g + "/wide.png", 1000, 300, (250, 250, 0))
EOF

rc=0
run() { echo "== $1"; shift; "$W/xmbwrap" --template "$TPL" "$@" || { echo "FAIL: xmbwrap exited $?"; rc=1; }; }
check() { python3 "$HERE/check_entry.py" "$TPL" "$@" || rc=1; }

run "inferred, GBAdhoc's art" --rom-file "$GB/roms/Test Game (USA).gba"
check "$CARD/PSP/GAME/Test_Game_USA" "Test Game (USA)" "Test Game (USA).gba" "PSP/GAME/GBAdhoc" 144x80 480x272

run "subfolder ROM, no art, own title" --rom-file "$GB/roms/sub/Other.gbc" --title "Pokémon Other" --no-art
check "$CARD/PSP/GAME/Pok_mon_Other" "Pokémon Other" "sub/Other.gbc" "PSP/GAME/GBAdhoc" - -

run "explicit root, own art, own folder" --root "$CARD" --rom "Test Game (USA).gba" --title "Wide" \
    --icon "$GB/wide.png" --background "$GB/wide.png" --folder MYGAME
check "$CARD/PSP/GAME/MYGAME" "Wide" "Test Game (USA).gba" "PSP/GAME/GBAdhoc" 144x80 480x272

echo "== a ROM outside an install is refused"
if "$W/xmbwrap" --template "$TPL" --rom-file "$W/elsewhere.gba" 2>/dev/null; then echo "FAIL: accepted"; rc=1; else echo "ok"; fi

echo "== PARAM.SFO matches the SDK's mksfoex for the same titles"
for t in "Test Game (USA)" "Wide"; do
  docker run --rm -v "$W":/w pspdev/pspdev mksfoex -d MEMSIZE=2 "$t" "/w/ref.sfo" >/dev/null 2>&1
  python3 "$HERE/check_entry.py" --sfo-ref "$W/ref.sfo" "$CARD/PSP/GAME/$( [ "$t" = Wide ] && echo MYGAME || echo Test_Game_USA )/EBOOT.PBP" || rc=1
done
[ $rc = 0 ] && echo "ALL PASS" || echo "FAILED"
exit $rc
