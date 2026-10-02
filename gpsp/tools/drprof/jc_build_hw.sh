#!/usr/bin/env bash
# jc_build_hw.sh OUT [kind ...] -- the PSP builds of the JIT_CODE_DISCIPLINE
# hardware plan (docs/JIT-CODE-DISCIPLINE.md section 8), each staged as
# OUT/<kind>/{EBOOT.PBP,gbadhoc_me.prx,build-manifest.json}.  Run from a
# CLEAN tree (a commit): the manifests record it.  Sequential (one tree).
#
#   jcoff     harness64, switch OFF (this tree's control; same as the candidate)
#   jcd       harness64 + JIT_CODE_DISCIPLINE=1 (the product)
#   jab-pN    harness64 + JIT_CODE_DISCIPLINE=1 JIT_CODE_AB=1, .text padded N
#             bytes (psp/layout_pad.S): one binary per layout, jit_code_mode
#             (0 legacy, 1 +8b48b48, 2 discipline, 3 no-owner) picked per arm
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT=$1; shift
PADS="0 1024 2560 3776 5120 6656"
KINDS="$*"
[ -n "$KINDS" ] || KINDS="jcoff jcd $(for p in $PADS; do printf 'jab-p%s ' "$p"; done)"
mkdir -p "$OUT"
for k in $KINDS; do
  case "$k" in
    jcoff) CE=""; PE="" ;;
    jcd)   CE="JIT_CODE_DISCIPLINE=1"; PE="" ;;
    jab-p*) CE="JIT_CODE_DISCIPLINE=1 JIT_CODE_AB=1"; PE="LAYOUT_PAD_TEXT=${k#jab-p}" ;;
    *) echo "unknown kind $k"; exit 2 ;;
  esac
  echo "=== $k  CORE_EXTRA='$CE' PSP_EXTRA='$PE'"
  ( cd "$ROOT" && CORE_EXTRA="$CE" PSP_EXTRA="$PE" bash tools/build.sh harness64 --out "$OUT/$k" ) \
     > "$OUT/$k.log" 2>&1 || { tail -30 "$OUT/$k.log"; exit 1; }
  grep -E 'elf md5|pbp md5' "$OUT/$k.log"
  # every build regenerates the tracked ME binaries; restore them so the next
  # build's manifest still records a clean tree (its PRX is staged in OUT)
  git -C "$ROOT" checkout -- psp/me 2>/dev/null || true
done
