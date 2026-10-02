#!/usr/bin/env bash
# jc_ppsspp.sh HW OUT -- the PPSSPP shash oracle of docs/JIT-CODE-DISCIPLINE.md
# (WSL, ~/ppsspp).  Runs the AW2 tour and the H&S heavy battle (resident: the
# harness64 default) on each PSP build under HW (jc_build_hw.sh output, plus
# HW/cdr = the candidate's own harness64 EBOOT) and every jit_code_mode of the
# A/B build, then compares every arm's shash with the candidate's, every
# frame and field (audio included: one platform).
#   FX=dir holding aw2.gba/.st0/.sav, hns.gba, heart_soul_heavy.st0, hns.sav,
#   aw2_psp_script.txt, battle.txt   (default ~/drprof/fx)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
HW=$1; OUT=$2
FX=${FX:-$HOME/drprof/fx}
RUN="$HERE/../linkbench/ppsspp_run.sh"
CMP="$HERE/../linkbench/cmp_hash.py"
mkdir -p "$OUT"
base_ini() {   # the drrig BASE_INI keys that matter off a console
  printf 'audio_oracle = 1\nme_mode = 1\nlog_input = 0\n'
  [ -n "${1:-}" ] && printf 'jit_code_mode = %s\n' "$1"
}
ARMS=${ARMS:-"cdr: jcoff: jcd: jab-p0:0 jab-p0:1 jab-p0:2 jab-p0:3"}
for fx in A H; do      # one fixture at a time: 7 PPSSPPs in parallel
  pids=()
  for arm in $ARMS; do
    b=${arm%%:*}; m=${arm#*:}
    tag="$fx-$b${m:+-m$m}"
    base_ini "$m" > "$OUT/$tag.ini"
    if [ $fx = A ]; then
      ( AUTOEXIT=7000 TIMEOUT=2400 bash "$RUN" "$tag" "$HW/$b" "$FX/aw2.gba" "$FX/aw2.st0" \
          "$FX/aw2.sav" "$FX/aw2_psp_script.txt" "$OUT" "$OUT/$tag.ini" > "$OUT/$tag.run" 2>&1 ) &
    else
      ( AUTOEXIT=3200 TIMEOUT=2400 bash "$RUN" "$tag" "$HW/$b" "$FX/hns.gba" "$FX/heart_soul_heavy.st0" \
          "$FX/hns.sav" "$FX/battle.txt" "$OUT" "$OUT/$tag.ini" > "$OUT/$tag.run" 2>&1 ) &
    fi
    pids+=($!)
  done
  wait "${pids[@]}"
done
for fx in A H; do
  for arm in $ARMS; do
    b=${arm%%:*}; m=${arm#*:}; tag="$fx-$b${m:+-m$m}"
    echo "== $tag: $(cat "$OUT/$tag.run" | tail -n 1)"
    grep -h 'jit_code mode\|core_health at=hb' "$OUT/$tag/frontend.log" 2>/dev/null | tail -n 2 | cut -c1-200
    [ "$b" = cdr ] && continue
    python3 "$CMP" "$OUT/$fx-cdr/shash.txt" "$OUT/$tag/shash.txt" 2>&1 | head -n 4
  done
done
