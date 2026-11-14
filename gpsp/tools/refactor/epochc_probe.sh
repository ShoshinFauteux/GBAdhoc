#!/usr/bin/env bash
# epochc_probe.sh -- Epoch C step C0a-6: build the section-placement probe and report.
#   bash gpsp/tools/refactor/epochc_probe.sh TREE WORK OUT [FLAVOUR]
#     TREE     a clean export of the commit to probe (the directory holding gpsp/)
#     WORK     scratch (WSL-native); a copy of TREE/gpsp is made here and patched
#     OUT      where report.<flavour>.md, objdump.txt and elf.nm are kept (small text files)
#     FLAVOUR  release (default) or lab
#   REF_NM=FILE   psp-nm -n of the same commit's unpatched build of that flavour (optional:
#                 the report then says how far main moved)
#   LOCK=FILE     the shared build lock (flock), default: none
#   C=, TR=       commit and tree ids for the build manifest (default: probe)
# WSL, docker (pspdev/pspdev, pinned by DOCKER_IMAGE if set).  The build is EXPECTED to
# stop at the layout lock (the probe moves code); the ELF and main_psp.o exist by then.
# The probe EBOOT is never kept: only the three text files leave WORK.
set -u
TREE=${1:?TREE}; WORK=${2:?WORK}; OUT=${3:?OUT}; F=${4:-release}
HERE=$(cd "$(dirname "$0")" && pwd)
IMG=${DOCKER_IMAGE:-pspdev/pspdev}
# the WSL disk image lives on C:; never build under the floor (FLOOR_GB, default 20)
if [ -d /mnt/c ]; then
  AV=$(df -BG --output=avail /mnt/c | tail -1 | tr -dc 0-9)
  [ "${AV:-0}" -ge "${FLOOR_GB:-20}" ] || { echo "epochc_probe: C: has ${AV} GB free (< ${FLOOR_GB:-20}): not building"; exit 7; }
fi
rm -rf "$WORK"; mkdir -p "$WORK" "$OUT"
cp -r "$TREE/gpsp" "$WORK/gpsp" || exit 9
python3 -I "$HERE/epochc_probe.py" patch "$WORK/gpsp/psp" || exit 1
cd "$WORK/gpsp" || exit 9
run() { if [ -n "${LOCK:-}" ]; then flock "$LOCK" "$@"; else "$@"; fi; }
if [ "$F" = release ]; then
  run env MSYS_NO_PATHCONV=1 GPSP_BUILD_COMMIT="${C:-probe}" GPSP_BUILD_TREE="${TR:-probe}" GPSP_BUILD_DIRTY=1 \
    RFU_SCHED=1 bash tools/build.sh release --out "$WORK/out" > "$WORK/build.log" 2>&1
else
  run env MSYS_NO_PATHCONV=1 GPSP_BUILD_COMMIT="${C:-probe}" GPSP_BUILD_TREE="${TR:-probe}" GPSP_BUILD_DIRTY=1 \
    CORE_EXTRA="RFU_SCHED=1" PSP_EXTRA="GBA_LINK=1 REWIND=1 ONLINE=1 RFU_SCHED=1 HOT_DATA_GUARD=1" \
    bash tools/build.sh lab --out "$WORK/out" > "$WORK/build.log" 2>&1
fi
echo "build rc=$? (a layout.lock stop is expected): $(grep -E 'FAIL|layout  :' "$WORK/build.log" | tail -2 | tr '\n' ' ')"
[ -f psp/main_psp.o ] && [ -f psp/gpsp_adhoc.elf ] || { echo "epochc_probe: no main_psp.o / ELF"; tail -20 "$WORK/build.log"; exit 1; }
docker run --rm -v "$WORK/gpsp:/build" -w /build/psp "$IMG" sh -c \
  'psp-objdump -h main_psp.o > /build/psp/probe_objdump.txt; psp-nm -n gpsp_adhoc.elf > /build/psp/probe_elf.nm' || exit 1
cp psp/probe_objdump.txt "$OUT/objdump.$F.txt"; cp psp/probe_elf.nm "$OUT/elf.$F.nm"
python3 -I "$HERE/epochc_probe.py" report --obj "$OUT/objdump.$F.txt" --nm "$OUT/elf.$F.nm" \
  ${REF_NM:+--base-nm "$REF_NM"} -o "$OUT/report.$F.md"
rc=$?
rm -rf "$WORK"
exit $rc
