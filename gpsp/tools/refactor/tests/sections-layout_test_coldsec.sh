#!/usr/bin/env bash
# sections-layout_test_coldsec.sh -- T10's input-section audit against the REAL
# toolchain: psp-gcc (pspdev/pspdev, the image the builds use) compiles a tiny
# stand-in TU named main_psp.c with -O2 -G0 -ffunction-sections (psp/Makefile's
# LAYOUT_PIN flags), links it with psp-gcc's default script and -Map, and
# layout_report.py audits that map.  No PSP build, no emulator source.
#
#   bash tests/sections-layout_test_coldsec.sh          (WSL or Git Bash, docker)
#   SL_LOCK=/path/build.lock bash ...                    serialise behind PSP builds
#
# Proves:
#   M1 a `cold` function with no section attribute is emitted as
#      .text.unlikely.<fn>, linked BEFORE .text.startup.main (it moves main), and
#      layout_report FAILS on it (exit 1)
#   C1 the same function with __attribute__((cold, section(".text.fe_cold_ok")))
#      is an ordinary input section and layout_report passes (exit 0)
#   M2 a second startup-phase function (.text.startup.fe_boot) fails unless the
#      plan lists it (C2: --plan with .text.startup.fe_* passes)
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
PY=python3; command -v python3 >/dev/null 2>&1 && python3 -c 1 2>/dev/null || PY=python
T=$(mktemp -d "${TMPDIR:-/tmp}/sl_cold.XXXXXX")
trap 'rm -rf "$T"' EXIT
FAILS=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
W="$T"; command -v cygpath >/dev/null 2>&1 && W=$(cygpath -w "$T")


mk() {  # mk <dir> <extra C>
  mkdir -p "$T/$1"
  cat > "$T/$1/main_psp.c" <<EOF
extern volatile int sink;
volatile int sink;
$2
__attribute__((noinline)) void frame(int i) { sink += i * 3; }
int main(int argc, char **argv) {
  for (int i = 0; i < argc; i++) frame(i);
  if (argc > 7) seeded(argc);
  return sink;
}
EOF
}
mk m1 '__attribute__((cold, noinline)) void seeded(int x) { sink = x * 7 + sink; }'
mk c1 '__attribute__((cold, noinline, section(".text.fe_cold_ok"))) void seeded(int x) { sink = x * 7 + sink; }'
mk m2 '__attribute__((noinline, section(".text.startup.fe_boot"))) void seeded(int x) { sink = x * 7 + sink; }'

run_docker() {
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' docker run --rm -v "$W":/t -w /t pspdev/pspdev sh -c '
    set -e
    for d in m1 c1 m2; do
      ( cd $d && psp-gcc -O2 -G0 -ffunction-sections -c -o main_psp.o main_psp.c &&
        psp-objdump -h main_psp.o | grep -o "\.text[^ ]*" > sections.txt &&
        psp-gcc -O2 -G0 -nostdlib -nostartfiles -Wl,-e,main -Wl,-Map=gpsp_adhoc.map -o gpsp_adhoc.elf main_psp.o )
    done'
}
if [ -n "${SL_LOCK:-}" ] && command -v flock >/dev/null 2>&1; then
  flock -w 900 "$SL_LOCK" bash -c "$(declare -f run_docker); W='$W' run_docker" > "$T/docker.log" 2>&1
else
  run_docker > "$T/docker.log" 2>&1
fi
for d in m1 c1 m2; do
  [ -f "$T/$d/gpsp_adhoc.elf" ] && [ -f "$T/$d/gpsp_adhoc.map" ] || { echo "toolchain step failed for $d:"; cat "$T/docker.log"; exit 2; }
done
echo "psp-gcc section names (main_psp.o of each variant):"
for d in m1 c1 m2; do echo "  $d: $(tr '\n' ' ' < "$T/$d/sections.txt")"; done

grep -q '^\.text\.unlikely\.seeded$' "$T/m1/sections.txt" && ok "M1 psp-gcc emits a section-less cold function as .text.unlikely.seeded" \
  || bad "M1 psp-gcc did not emit .text.unlikely.seeded"
# placement: is .text.unlikely.seeded linked before .text.startup.main?
ua=$(grep -A1 '^ \.text\.unlikely\.seeded' "$T/m1/gpsp_adhoc.map" | grep -o '0x[0-9a-f]\{8\}' | head -1)
ma=$(grep -A1 '^ \.text\.startup\.main' "$T/m1/gpsp_adhoc.map" | grep -o '0x[0-9a-f]\{8\}' | head -1)
if [ -n "$ua" ] && [ -n "$ma" ] && [ $((ua)) -lt $((ma)) ]; then ok "M1 the default script links .text.unlikely.seeded ($ua) before main ($ma)"
else bad "M1 placement: unlikely=$ua main=$ma"; fi

expect() {  # expect <rc> <label> <args...>
  local want="$1" label="$2"; shift 2
  "$PY" "$HERE/layout_report.py" --no-pad-check "$@" > "$T/last.log" 2>&1; local rc=$?
  if [ "$rc" -eq "$want" ]; then ok "$label (exit $rc)"; else bad "$label: exit $rc, wanted $want"; tail -12 "$T/last.log" | sed 's/^/        /'; fi
}
expect 1 "M1 layout_report fails on the cold function without a section" "$T/m1/gpsp_adhoc.elf" --map "$T/m1/gpsp_adhoc.map"
grep -q 'UNPLANNED .text.unlikely.seeded' "$T/last.log" && ok "M1 the report names .text.unlikely.seeded" || bad "M1 not named"
expect 0 "C1 layout_report passes the cold function with an explicit section" "$T/c1/gpsp_adhoc.elf" --map "$T/c1/gpsp_adhoc.map"
expect 1 "M2 an unplanned .text.startup.fe_boot fails" "$T/m2/gpsp_adhoc.elf" --map "$T/m2/gpsp_adhoc.map"
printf '.text.startup.main\n.text.startup.fe_*\n' > "$T/plan.txt"
expect 0 "C2 ...and passes once the plan lists .text.startup.fe_*" "$T/m2/gpsp_adhoc.elf" --map "$T/m2/gpsp_adhoc.map" --plan "$T/plan.txt"
echo "sections-layout_test_coldsec: $([ $FAILS -eq 0 ] && echo PASS || echo "$FAILS FAILED")"
exit $((FAILS > 0))
