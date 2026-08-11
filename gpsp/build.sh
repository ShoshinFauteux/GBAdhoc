#!/usr/bin/env bash
# build.sh — build core + EBOOT and PROVE it happened.
#
# Written after two silent no-op builds today: one produced a byte-identical
# EBOOT while the source was 27 minutes newer, and one failed with "no
# makefile found" because $PWD did not survive the Git Bash -> WSL boundary.
# Both were invisible behind an output filter.  So this script cds by absolute
# path, never filters the build output it depends on, and ends by looking for
# named symbols inside the object file rather than trusting that make ran.
set -uo pipefail
cd "$(dirname "$0")" || exit 9
ROOT="$PWD"
echo "=== building in $ROOT ==="

# Git Bash rewrites any argument that looks like a unix path, so docker's
# `-w /build` arrived as `C:/Program Files/Git/build` and every container died
# with "working directory is invalid".  The [ -f ] checks below then passed on
# the PREVIOUS build's artifacts and the script reported success with an
# unchanged md5 -- the exact class of failure this file exists to prevent.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

# The artifact must end up NEWER than every source, so a container that fails
# cannot be papered over by a leftover file.  Recorded before anything runs.
NEWEST_SRC=$(find "$ROOT" -name '*.c' -o -name '*.h' -o -name 'Makefile' \
             | xargs ls -t 2>/dev/null | head -1)

run() {   # run <label> <workdir> <cmd...>; abort on a nonzero container
  local label="$1" wd="$2"; shift 2
  local outp
  outp=$(docker run --rm -v "$ROOT":/build -w "$wd" pspdev/pspdev sh -c "$*" 2>&1)
  local rc=$?
  printf '%s\n' "$outp" | tail -3
  [ "$rc" -eq 0 ] || { echo "FAIL: $label container exited $rc"; exit 1; }
}

run "core"  /build      'make platform=psp1 -j4'
[ -f "$ROOT/gpsp_libretro_psp1.a" ] || { echo "FAIL: core archive missing"; exit 1; }

run "eboot" /build/psp  'make'
[ -f "$ROOT/psp/EBOOT.PBP" ] || { echo "FAIL: EBOOT missing"; exit 1; }

if [ -n "$NEWEST_SRC" ] && [ "$NEWEST_SRC" -nt "$ROOT/psp/EBOOT.PBP" ]; then
  echo "FAIL: EBOOT.PBP is OLDER than $NEWEST_SRC -- build did not run"
  exit 1
fi

echo "=== proof ==="
# Search the LINKED BINARY, not object symbol tables.  psp-nm only lists two
# objects and only lists symbols, so it silently missed a static function and a
# changed format string -- and `strings` does not exist in Git Bash, so a
# `strings | grep` check read an empty stream and reported every token missing
# while the build was in fact correct.  Both directions of that failure were
# observed.  Reading the bytes answers for symbols and literals alike.
# The interpreter is Windows python; it cannot open an MSYS "/c/..." path and
# reports the file missing, which reads identically to a build that produced
# nothing.  Hand it a native path.
ELF="$ROOT/psp/gpsp_adhoc.elf"
[ -f "$ELF" ] || ELF="$ROOT/psp/EBOOT.PBP"
command -v cygpath >/dev/null && ELF=$(cygpath -w "$ELF")
python - "$ELF" "$@" <<'PY'
import sys
path, toks = sys.argv[1], sys.argv[2:]
try:
    blob = open(path, 'rb').read()
except OSError as e:
    print("  FAIL cannot read %s: %s" % (path, e)); sys.exit(1)
if not toks:
    print("  (no tokens given -- pass what this build was supposed to add)")
rc = 0
for t in toks:
    if t.encode() in blob:
        print("  OK   present: %s" % t)
    else:
        print("  FAIL MISSING: %s" % t); rc = 1
sys.exit(rc)
PY
[ $? -eq 0 ] || exit 1
echo "md5: $(md5sum "$ROOT/psp/EBOOT.PBP" | cut -d' ' -f1)"
