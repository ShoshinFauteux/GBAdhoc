#!/usr/bin/env bash
# sections-layout_test_regen.sh -- T5's opt-in wiring in tools/layout/regen.sh,
# with NO PSP build and NO docker: a throwaway copy of the layout tools, the
# baseline release ELF, and a fake `docker` that writes the three nm files the
# real container would (elf.nm from the flavour dir, mips_stub.nm and lib.nm
# derived from its link map and hot_order.txt).
#
#   bash tests/sections-layout_test_regen.sh [FLAVOUR_DIR]
#   (default: mainpsp-tools-out/baseline/run2/release beside the checkout; local)
#
# Proves:
#   C1 plain `regen.sh` (no flag) runs as before and never calls hotord_check
#   M1 --check-only on the committed order fails (exit 1): today's order is stale
#   C2 --check-only on an order regenerated but NOT relinked fails on placement
#      (exit 1, PLACE): the ELF does not follow the new order yet
#   M2 --check-only with lat_swap.isra.0 renamed .isra.1 in the ELF fails (exit 1, RENAME)
#   C3 --check regenerates and the new order passes the post-check (exit 0)
#   M3 --check where the nm the order is generated from disagrees with the ELF
#      (update_gba +64 B) fails the post-check (exit 1)
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)            # gpsp/tools/refactor
GPSP=$(cd "$HERE/../.." && pwd)
FD=${1:-$(cd "$GPSP/../../mainpsp-tools-out/baseline/run2/release" 2>/dev/null && pwd)}
[ -f "$FD/gpsp_adhoc.elf" ] && [ -f "$FD/elf.nm" ] && [ -f "$FD/gpsp_adhoc.map" ] || {
  echo "need a flavour dir with gpsp_adhoc.elf, elf.nm and gpsp_adhoc.map (got '$FD')"; exit 2; }
PY=python3; command -v python3 >/dev/null 2>&1 && python3 -c 1 2>/dev/null || PY=python
T=$(mktemp -d "${TMPDIR:-/tmp}/sl_regen.XXXXXX")
trap 'rm -rf "$T"' EXIT
FAILS=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
expect_rc() {   # expect_rc <want> <label> <grep-or-empty> <cmd...>
  local want="$1" label="$2" pat="$3"; shift 3
  "$@" > "$T/last.log" 2>&1; local rc=$?
  if [ "$rc" -eq "$want" ] && { [ -z "$pat" ] || grep -q -- "$pat" "$T/last.log"; }; then
    ok "$label (exit $rc)"
  else
    bad "$label: exit $rc, wanted $want${pat:+ and '$pat'}"; sed 's/^/        /' "$T/last.log" | tail -12
  fi
}

# --- the throwaway tree (only what regen.sh and the checker read)
R="$T/gpsp"
mkdir -p "$R/tools/layout" "$R/tools/refactor" "$R/psp/layout" "$T/bin" "$T/fx"
cp "$GPSP"/tools/layout/{regen.sh,gen_layout.py,lp_map.py,profile_weights.txt,psp_frame_path.txt} "$R/tools/layout/"
cp "$HERE"/{hotord_check.py,elf32.py,sl_ldmap.py} "$R/tools/refactor/"
cp "$GPSP"/psp/layout/{hot_order.txt,hot.ord} "$R/psp/layout/"
cp "$FD/gpsp_adhoc.elf" "$R/psp/gpsp_adhoc.elf"
: > "$R/gpsp_libretro_psp1.a"
cp "$GPSP/psp/layout/hot_order.txt" "$T/fx/hot_order.committed"
cp "$GPSP/psp/layout/hot.ord" "$T/fx/hot.ord.committed"

# --- fixtures the fake container writes
cat > "$T/fx/mk.py" <<'EOF'
import os, re, sys
sys.path.insert(0, sys.argv[1])
import sl_ldmap
fd, out, order = sys.argv[2], sys.argv[3], sys.argv[4]
m = sl_ldmap.LdMap(os.path.join(fd, 'gpsp_adhoc.map'))
# mips_stub.o's symbols as psp-nm of the object prints them (labels included):
# every text symbol of elf.nm inside one of instance A's mips_stub.o sections
iv = [(i.addr, i.addr + i.size) for i in m.inputs
      if i.file.endswith('gpsp_libretro_psp1.a(mips_stub.o)') and i.out == '.text' and i.size]
with open(os.path.join(out, 'mips_stub.nm'), 'w', newline='\n') as f:
    for l in open(os.path.join(fd, 'elf.nm')):
        p = l.split()
        if len(p) >= 3 and p[-2] in 'tT':
            a = int(p[0], 16)
            if any(lo <= a < hi for lo, hi in iv):
                f.write('%08x %s %s\n' % (a - 0x08804040, p[-2], p[-1]))
with open(os.path.join(out, 'lib.nm'), 'w', newline='\n') as f:
    for l in open(order):
        p = l.split('#', 1)[0].split()
        if len(p) > 3 and ':' in p[3]:
            lib, mem = p[3].split(':', 1)
            f.write('/usr/local/pspdev/psp/lib/%s:%s:00000000 T %s\n' % (lib, mem, p[0]))
EOF
"$PY" "$T/fx/mk.py" "$HERE" "$FD" "$T/fx" "$GPSP/psp/layout/hot_order.txt" || { echo "fixture build failed"; exit 2; }
cp "$FD/elf.nm" "$T/fx/elf.nm"
# M3's nm: update_gba 64 B bigger than the ELF says
sed -E 's/^([0-9a-f]{8}) ([0-9a-f]{8}) T update_gba$/\1 SIZEX T update_gba/' "$FD/elf.nm" > "$T/fx/elf.nm.bad"
sz=$(grep -E ' T update_gba$' "$FD/elf.nm" | awk '{print $2}')
nsz=$(printf '%08x' $((16#$sz + 64)))
sed -i "s/ SIZEX T update_gba\$/ $nsz T update_gba/" "$T/fx/elf.nm.bad"

cat > "$T/bin/docker" <<EOF
#!/bin/sh
echo "docker \$*" >> "$T/docker.log"
[ "\$1" = run ] || exit 0
mkdir -p .layout
cp "\${FAKE_ELF_NM:-$T/fx/elf.nm}" .layout/elf.nm
cp "$T/fx/mips_stub.nm" .layout/mips_stub.nm
cp "$T/fx/lib.nm" .layout/lib.nm
EOF
chmod +x "$T/bin/docker"
export PATH="$T/bin:$PATH"
reset_order() { cp "$T/fx/hot_order.committed" "$R/psp/layout/hot_order.txt"; cp "$T/fx/hot.ord.committed" "$R/psp/layout/hot.ord"; }

echo "regen.sh T5 wiring (flavour dir $FD)"
# C1 plain regen.sh
reset_order; : > "$T/docker.log"
expect_rc 0 "C1 plain regen.sh regenerates" "wrote" bash "$R/tools/layout/regen.sh"
if grep -q hotord_check "$T/last.log"; then bad "C1 plain regen.sh ran hotord_check"; else ok "C1 plain regen.sh did not run hotord_check"; fi
grep -q '^docker run' "$T/docker.log" && ok "C1 plain regen.sh used the (fake) container" || bad "C1 no docker run"
cp "$R/psp/layout/hot_order.txt" "$T/fx/hot_order.regen"; cp "$R/psp/layout/hot.ord" "$T/fx/hot.ord.regen"
# M1 committed order, --check-only
reset_order; : > "$T/docker.log"
expect_rc 1 "M1 --check-only on the committed (stale) order fails" "ROUNDING" bash "$R/tools/layout/regen.sh" --check-only
[ -s "$T/docker.log" ] && bad "M1 --check-only ran docker" || ok "M1 --check-only needs no docker"
# C2 the regenerated order passes --check-only
cp "$T/fx/hot_order.regen" "$R/psp/layout/hot_order.txt"; cp "$T/fx/hot.ord.regen" "$R/psp/layout/hot.ord"
expect_rc 1 "C2 --check-only on a regenerated, not relinked order fails on placement" "PLACE" bash "$R/tools/layout/regen.sh" --check-only
# M2 lat_swap re-cloned in the ELF (only meaningful if the order lists it)
if grep -q '^lat_swap.isra.0 ' "$R/psp/layout/hot_order.txt"; then
  "$PY" -c "import sys;p=sys.argv[1];d=open(p,'rb').read();open(p,'wb').write(d.replace(b'\0lat_swap.isra.0\0',b'\0lat_swap.isra.1\0'))" "$R/psp/gpsp_adhoc.elf"
  expect_rc 1 "M2 --check-only, lat_swap.isra.0 renamed .isra.1 in the ELF" "RENAME" bash "$R/tools/layout/regen.sh" --check-only
  cp "$FD/gpsp_adhoc.elf" "$R/psp/gpsp_adhoc.elf"
  # and the committed order too (what a build of a renaming commit would hit)
  reset_order
  "$PY" -c "import sys;p=sys.argv[1];d=open(p,'rb').read();open(p,'wb').write(d.replace(b'\0lat_swap.isra.0\0',b'\0lat_swap.isra.1\0'))" "$R/psp/gpsp_adhoc.elf"
  expect_rc 1 "M2b same rename against the committed order" "lat_swap.isra.0 .*renamed: now lat_swap.isra.1" bash "$R/tools/layout/regen.sh" --check-only
  cp "$FD/gpsp_adhoc.elf" "$R/psp/gpsp_adhoc.elf"
else
  bad "M2 the regenerated order does not list lat_swap.isra.0 (cannot run the rename mutant)"
fi
# C3 --check: stale before (informational), regenerated, post-check passes
reset_order
expect_rc 0 "C3 --check regenerates and the new order passes" "regenerated order against" bash "$R/tools/layout/regen.sh" --check
grep -q "ROUNDING" "$T/last.log" && ok "C3 the (non-fatal) pre-check reported the committed order's rounding breaks" || bad "C3 the pre-check reported nothing"
# M3 --check where the nm disagrees with the ELF
reset_order
FAKE_ELF_NM="$T/fx/elf.nm.bad" expect_rc 1 "M3 --check, nm says update_gba +64 B: post-check fails" "fails T5" bash "$R/tools/layout/regen.sh" --check
echo "sections-layout_test_regen: $([ $FAILS -eq 0 ] && echo PASS || echo "$FAILS FAILED")"
exit $((FAILS > 0))
