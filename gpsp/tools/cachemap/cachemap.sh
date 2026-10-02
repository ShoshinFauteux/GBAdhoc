#!/usr/bin/env bash
# cachemap.sh ELF PROFILE_DIR OUTDIR [cachemap.py args...]
#   -- the instruction-cache map of a linked PSP ELF (docs/CACHE-MAP.md).
#
#   ELF          psp/gpsp_adhoc.elf of the build to map (psp/Makefile builds
#                -g; the EBOOT is packed stripped, so that costs nothing)
#   PROFILE_DIR  twin_profile.sh's output (icount.txt, linkplay, run.log)
#   OUTDIR       gets the symbol/line dumps, report.txt, sets.csv,
#                lines.txt (per-line heat), map.json
# The per-frame PSP path the twin cannot run comes from
# tools/cachemap/psp_frame_path.txt unless --frame-path is given.
# Run from WSL/Linux or Git Bash with docker.
set -euo pipefail
ELF="$1"; PROF="$2"; OUT="$3"; shift 3
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
ELF_DIR="$(cd "$(dirname "$ELF")" && pwd)"; ELF_B="$(basename "$ELF")"
PROF="$(cd "$PROF" && pwd)"; OUT="$(cd "$OUT" && pwd)"
dpath() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else echo "$1"; fi; }
export MSYS_NO_PATHCONV=1
docker run --rm -v "$(dpath "$ELF_DIR")":/e -v "$(dpath "$OUT")":/o pspdev/pspdev \
  sh -c "psp-nm -n -S /e/$ELF_B > /o/elf.nm && psp-objdump --dwarf=decodedline /e/$ELF_B > /o/elf.lines"
docker run --rm -v "$(dpath "$PROF")":/p -v "$(dpath "$OUT")":/o drprof-qemu \
  sh -c "mipsel-linux-gnu-nm -n -S /p/linkplay > /o/twin.nm && mipsel-linux-gnu-objdump --dwarf=decodedline /p/linkplay > /o/twin.lines"
FR=$(grep -o 'lp_end frames=[0-9]*' "$PROF/run.log" | head -1 | grep -o '[0-9]*$' || echo 1)
WARM=${WARM:-120}
FR=$((FR > WARM ? FR - WARM : FR))
FP=()
case " $* " in *" --frame-path "*) ;; *) FP=(--frame-path "$HERE/psp_frame_path.txt") ;; esac
PY=python3; command -v python3 >/dev/null || PY=python
$PY "$HERE/cachemap.py" --elf-nm "$OUT/elf.nm" --elf-lines "$OUT/elf.lines" \
  --twin-nm "$OUT/twin.nm" --twin-lines "$OUT/twin.lines" \
  --icount "$PROF/icount.txt" --frames "$FR" "${FP[@]}" \
  --csv "$OUT/sets.csv" --lines-out "$OUT/lines.txt" --json "$OUT/map.json" "$@" \
  | tee "$OUT/report.txt"
