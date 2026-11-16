#!/usr/bin/env bash
# splice_sound_driver.sh -- build bios/source/sound_driver.c and splice it into
# bios/open_gba_bios.bin (see bios/README.md).  Run in WSL/Linux with docker.
#
# The rest of the image was built long ago with devkitARM and is kept byte for
# byte: rebuilding it with another compiler would move every SWI for no gain.
# The driver is compiled on its own (ARMv4T, ARM state, freestanding, no
# library) and linked at 0x2800, into the zero tail of the image.
#
#   SD_IMAGE   docker image with arm-linux-gnueabihf-gcc (default upstream-prs-cross)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE=${SD_IMAGE:-upstream-prs-cross}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cp "$HERE/source/sound_driver.c" "$HERE/sound_driver.ld" "$W/"
docker run --rm -v "$W":/w -w /w --entrypoint /bin/sh "$IMAGE" -c '
  set -e
  P=arm-linux-gnueabihf-
  ${P}gcc --version | head -1
  ${P}gcc -march=armv4t -marm -mfloat-abi=soft -O2 -ffreestanding -fno-builtin \
    -fno-pic -fno-pie -fno-stack-protector -fno-tree-loop-distribute-patterns \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra -Werror \
    -c sound_driver.c -o sd.o
  ${P}ld -nostdlib -static -T sound_driver.ld -o sd.elf sd.o
  ${P}nm sd.elf > sd.nm
  ${P}objdump -h sd.elf | grep -E "\.(data|bss) " | awk "\$3 != \"00000000\" {print; bad=1} END {exit bad}"
  if ${P}nm -u sd.o | grep -q .; then ${P}nm -u sd.o; echo "undefined symbols"; exit 1; fi
  ${P}objcopy -O binary -j .text sd.elf sd.bin
  ${P}objdump -d sd.elf > sd.dis
  chmod a+rw sd.*'
python3 -I "$HERE/splice_sound_driver.py" "$HERE/open_gba_bios.bin" "$W/sd.bin" "$W/sd.nm"
if [ -n "${SD_KEEP:-}" ]; then cp "$W"/sd.dis "$W"/sd.nm "$SD_KEEP"/; fi
