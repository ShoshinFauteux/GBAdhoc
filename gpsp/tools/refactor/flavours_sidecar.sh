#!/bin/bash
# flavours_sidecar.sh -- runs INSIDE the pspdev container, called by flavours.sh
# right after tools/build.sh finished one flavour and before anything cleans.
#
# It produces the two artifacts the normal build does not, WITHOUT changing the
# build: the linker map and -fstack-usage .su files (PLAN.md §5 T12/G3).
#
#   * .su files: every compile line make echoed (.build-core.log in /build,
#     .build-eboot.log in /build/psp, following "Entering directory") is
#     replayed with -fstack-usage and its -o redirected into /out/sidecar/obj.
#     Each replayed object is compared with the real one: byte-identical, or
#     identical after --strip-debug on both (-fstack-usage is recorded in
#     DW_AT_producer, .debug_str only).  Anything else is a DIFF and fails.
#     No DIFF means -fstack-usage changed no loadable byte of any object.
#   * the map: the gpsp_adhoc.elf link line is replayed with -Wl,-Map=... into
#     /out/sidecar/relink.elf, psp-fixup-imports is run on it as build.mak does,
#     and it must equal /build/psp/gpsp_adhoc.elf byte for byte.
#
# Mounts: /build = the gpsp tree just built, /out = the flavour's output dir.
# SOURCE_DATE_EPOCH must be the value the build used (harness/lab compile
# __DATE__/__TIME__ into main_psp.o, main_psp.c:13926).
#
# usage: flavours_sidecar.sh all            (from flavours.sh)
#        flavours_sidecar.sh one WD LINE    (internal, one compile, xargs -P)
set -u
SC=/out/sidecar

one() {   # one <wd> <compile line>
  local wd="$1" line="$2" obj abs rel dst new
  obj=$(printf '%s\n' "$line" | sed -nE 's/.* -o ([^ ]+\.o)( .*|$)/\1/p')
  [ -n "$obj" ] || { echo "SKIP no-object: $line"; return 0; }
  [ -f "$wd/$obj" ] || { echo "MISSING $wd/$obj"; return 0; }
  abs=$(realpath "$wd/$obj"); rel=${abs#/build/}    # busybox realpath: no -m
  dst="$SC/obj/$rel"; mkdir -p "$(dirname "$dst")"
  new=$(printf '%s\n' "$line" | sed -E "s# -o [^ ]+\.o( |\$)# -fstack-usage -o $dst\1#")
  if ! (cd "$wd" && eval "$new") >"$dst.log" 2>&1; then
    # build.mak deletes the generated PRX exports.c after compiling it; those
    # objects are PRX-only (never in gpsp_adhoc.elf), so not a failure
    if grep -q "fatal error: .*: No such file or directory" "$dst.log"; then
      echo "SKIP-GENERATED $rel"
    else
      echo "COMPILE-FAIL $rel"
    fi
    return 0
  fi
  # -fstack-usage is recorded in DW_AT_producer (.debug_str), so a replayed
  # object differs in DWARF only.  SAME = byte-identical; SAME-NODEBUG = equal
  # once both copies are --strip-debug'ed (code, data, relocations, symbols
  # all identical); DIFF = anything else, a real change.
  if cmp -s "$dst" "$abs"; then echo "SAME $rel"; return 0; fi
  psp-objcopy --strip-debug "$dst" "$dst.nd" && psp-objcopy --strip-debug "$abs" "$dst.real.nd" &&
  if cmp -s "$dst.nd" "$dst.real.nd"; then echo "SAME-NODEBUG $rel"; else echo "DIFF $rel"; fi
  rm -f "$dst.nd" "$dst.real.nd"
}

if [ "${1:-}" = one ]; then one "$2" "$3"; exit 0; fi
[ "${1:-}" = all ] || { echo "usage: $0 all"; exit 2; }

rm -rf "$SC"; mkdir -p "$SC/obj"
: "${SOURCE_DATE_EPOCH:?flavours.sh must pass SOURCE_DATE_EPOCH (or FLAV_NO_SDE=1)}"
[ "$SOURCE_DATE_EPOCH" = none ] && unset SOURCE_DATE_EPOCH

# wd<TAB>line for every compile command, tracking make's directory changes.
emit() {   # emit <log> <start wd>
  tr -d '\r' < "$1" | awk -v wd="$2" '
    /^make(\[[0-9]+\])?: Entering directory / { s=$0; sub(/.*directory ./, "", s); sub(/.$/, "", s); stack[++n]=wd; wd=s; next }
    /^make(\[[0-9]+\])?: Leaving directory /  { if (n>0) wd=stack[n--]; next }
    /^(psp-gcc|psp-g\+\+) / && / -c / && / -o [^ ]+\.o( |$)/ { print wd "\t" $0 }'
}
{ emit /build/.build-core.log /build; emit /build/.build-eboot.log /build/psp; } > "$SC/compiles.tsv"
NC=$(wc -l < "$SC/compiles.tsv")
echo "sidecar: $NC compile lines"
# xargs -P: each worker gets wd and line as two args
awk -F'\t' '{ printf "%s%c%s%c", $1, 0, $2, 0 }' "$SC/compiles.tsv" |
  xargs -0 -n2 -P "${SIDECAR_JOBS:-6}" bash "$0" one > "$SC/objects.txt" 2>&1
sort -o "$SC/objects.txt" "$SC/objects.txt"
SAME=$(grep -c '^SAME ' "$SC/objects.txt"); SAMEND=$(grep -c '^SAME-NODEBUG ' "$SC/objects.txt")
DIFF=$(grep -c '^DIFF ' "$SC/objects.txt"); SKIP=$(grep -c '^SKIP-GENERATED ' "$SC/objects.txt")
BAD=$(grep -cvE '^(SAME|SAME-NODEBUG|DIFF|SKIP-GENERATED) ' "$SC/objects.txt")
echo "sidecar: objects SAME=$SAME SAME-NODEBUG=$SAMEND DIFF=$DIFF SKIP-GENERATED=$SKIP other=$BAD"
SAME=$((SAME + SAMEND))

# collect .su into /out/su, mirroring the object paths
rm -rf /out/su; mkdir -p /out/su
( cd "$SC/obj" && find . -name '*.su' | while read -r f; do
    mkdir -p "/out/su/$(dirname "$f")"; cp "$f" "/out/su/$f"; done )
NSU=$(find /out/su -name '*.su' | wc -l)
echo "sidecar: $NSU .su files"

# the map, by relinking
LINK=$(tr -d '\r' < /build/.build-eboot.log | grep -E '^psp-gcc .* -o gpsp_adhoc\.elf( |$)' | tail -1)
RELINK=FAIL
if [ -n "$LINK" ]; then
  NEWL=$(printf '%s\n' "$LINK" | sed -E "s# -o gpsp_adhoc\.elf( |\$)# -Wl,-Map=$SC/gpsp_adhoc.map -o $SC/relink.elf\1#")
  if (cd /build/psp && eval "$NEWL" && psp-fixup-imports "$SC/relink.elf") > "$SC/relink.log" 2>&1; then
    if grep -q "could not fixup imports\|stubs out of order" "$SC/relink.log"; then RELINK=FIXUP-WARNING
    elif cmp -s "$SC/relink.elf" /build/psp/gpsp_adhoc.elf; then RELINK=SAME; cp "$SC/gpsp_adhoc.map" /out/gpsp_adhoc.map
    else RELINK=DIFF; fi
  fi
else
  echo "sidecar: no gpsp_adhoc.elf link line in .build-eboot.log"
fi
echo "sidecar: relink-with-map $RELINK"

psp-size /build/psp/gpsp_adhoc.elf > /out/size.txt 2>&1
psp-size -A -x /build/psp/gpsp_adhoc.elf > /out/size-A.txt 2>&1
psp-nm -S -n /build/psp/gpsp_adhoc.elf > /out/elf.nm 2>/dev/null

# drop the replayed objects (big); keep .su, logs and the verdicts
find "$SC/obj" -name '*.o' -delete; rm -f "$SC/relink.elf"
printf 'objects_same_bytes=%s\nobjects_same_nodebug=%s\nobjects_diff=%s\nobjects_skip_generated=%s\nobjects_other=%s\nsu_files=%s\nrelink=%s\n' \
  "$((SAME - SAMEND))" "$SAMEND" "$DIFF" "$SKIP" "$BAD" "$NSU" "$RELINK" > /out/sidecar.txt
# the container runs as root: hand the outputs back, so the next run can clear them
[ -n "${HOST_UID:-}" ] && chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" /out
[ "$DIFF" -eq 0 ] && [ "$BAD" -eq 0 ] && [ "$RELINK" = SAME ] && [ "$SAME" -gt 0 ]
