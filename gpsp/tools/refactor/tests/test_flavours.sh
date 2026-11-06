#!/usr/bin/env bash
# test_flavours.sh -- self-test of flavours.sh with NO PSP build: a throwaway
# git repo whose gpsp/tools/build.sh is a fake, and a fake `docker`.
# Run from WSL or Linux (needs git, flock not required):  bash tests/test_flavours.sh
#
# Proves (each a mutant that must be caught, or a control that must pass):
#   C1 clean --src build passes; workingTreeClean true; SOURCE_DATE_EPOCH reached
#      every `docker run` build.sh made (via the shim); the stamp check passes
#   M1 a dirty tree with --baseline is refused (exit 3) before any build
#   M2 "could not fixup imports" in a make log fails the run (exit 4), although
#      the fake build.sh exits 0 -- the psp-stub-order-trap
#   M3 a docker that drops SOURCE_DATE_EPOCH is caught by the stamp check (exit 1)
#   C2 --commit HEAD exports the COMMITTED tree: a dirty edit is not built
#   M4 a failing build.sh fails the run (exit 1)
#   M5 an unknown flavour is a usage error (exit 2)
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/flavtest.XXXXXX")
trap 'rm -rf "$T"' EXIT
FAILS=0
ok()   { echo "  ok    $*"; }
bad()  { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }
expect_rc() {   # expect_rc <want> <label> <cmd...>
  local want="$1" label="$2"; shift 2
  "$@" > "$T/last.log" 2>&1; local rc=$?
  if [ "$rc" -eq "$want" ]; then ok "$label (exit $rc)"; else bad "$label: exit $rc, wanted $want"; sed 's/^/        /' "$T/last.log" | tail -8; fi
}

# --- fake docker: records argv; on run, writes the __DATE__/__TIME__ the compiler would see
mkdir -p "$T/bin"
cat > "$T/bin/docker" <<'EOF'
#!/bin/sh
echo "$*" >> "$FAKE_DOCKER_LOG"
[ "$1" = run ] || exit 0
sde=""
for a in "$@"; do case "$a" in SOURCE_DATE_EPOCH=*) sde=${a#SOURCE_DATE_EPOCH=} ;; esac; done
[ -n "${FAKE_NOSDE:-}" ] && sde=""
if [ -n "$sde" ]; then
  printf '%s|%s' "$(LC_ALL=C date -u -d "@$sde" '+%b %e %Y')" "$(LC_ALL=C date -u -d "@$sde" '+%H:%M:%S')" > "$FAKE_STAMP"
else
  printf '%s|%s' "$(LC_ALL=C date -u '+%b %e %Y')" "99:99:99" > "$FAKE_STAMP"
fi
EOF
chmod +x "$T/bin/docker"
export PATH="$T/bin:$PATH" FAKE_DOCKER_LOG="$T/docker.log" FAKE_STAMP="$T/stamp"

# --- fake repo: gpsp/tools/build.sh + the real flavours scripts under test
R="$T/repo"; mkdir -p "$R/gpsp/tools/refactor/tests" "$R/gpsp/psp"
cp "$HERE/flavours.sh" "$HERE/flavours_sidecar.sh" "$R/gpsp/tools/refactor/"
cat > "$R/gpsp/tools/build.sh" <<'EOF'
#!/usr/bin/env bash
# fake build.sh: one docker run, an ELF holding the stamp and the source marker
cd "$(dirname "$0")/.." || exit 9
[ -n "${FAKE_FAIL:-}" ] && { echo "error: fake failure"; exit 2; }
docker run --rm -v "$PWD":/build -w /build pspdev/pspdev sh -c "make"
echo "psp-gcc -c -o x.o x.c" > .build-core.log
if [ -n "${FAKE_FIXUP:-}" ]; then
  echo "Warning: could not fixup imports, stubs out of order." > .build-eboot.log
else
  echo "psp-fixup-imports gpsp_adhoc.elf" > .build-eboot.log
fi
{ printf 'ELF '; tr '|' '\n' < "$FAKE_STAMP"; printf '\n'; cat marker.txt; } > psp/gpsp_adhoc.elf
cp psp/gpsp_adhoc.elf psp/EBOOT.PBP
echo "{\"profile\":\"$1\",\"dirty\":\"${GPSP_BUILD_DIRTY:-unset}\"}" > psp/build-manifest.json
exit 0
EOF
echo committed > "$R/gpsp/marker.txt"; : > "$R/gpsp/psp/.keep"
printf 'psp/gpsp_adhoc.elf\npsp/EBOOT.PBP\npsp/build-manifest.json\n.build-*.log\n' > "$R/gpsp/.gitignore"
(cd "$R" && git init -q && git add -A && git -c user.email=t@t -c user.name=t commit -q -m base) || { echo "git unavailable"; exit 2; }
F="$R/gpsp/tools/refactor/flavours.sh"
FL="release harness"

echo "test_flavours: $T"
# C1
: > "$FAKE_DOCKER_LOG"
expect_rc 0 "C1 clean --src --baseline builds" bash "$F" --src "$R/gpsp" --out "$T/o1" --baseline --no-sidecars --flavours "$FL"
grep -q '"workingTreeClean": true' "$T/o1/harness/flavour.json" 2>/dev/null && ok "C1 workingTreeClean recorded true" || bad "C1 workingTreeClean not true"
N_RUN=$(grep -c '^run ' "$FAKE_DOCKER_LOG"); N_SDE=$(grep -c '^run -e SOURCE_DATE_EPOCH=' "$FAKE_DOCKER_LOG")
[ "$N_RUN" -gt 0 ] && [ "$N_RUN" -eq "$N_SDE" ] && ok "C1 every docker run got SOURCE_DATE_EPOCH ($N_SDE/$N_RUN)" || bad "C1 shim: $N_SDE of $N_RUN docker runs had SOURCE_DATE_EPOCH"
grep -q '"buildStamp": "pinned' "$T/o1/harness/flavour.json" 2>/dev/null && ok "C1 harness stamp check passed" || bad "C1 harness stamp not pinned"
# M1
echo dirty >> "$R/gpsp/marker.txt"
: > "$FAKE_DOCKER_LOG"
expect_rc 3 "M1 dirty tree + --baseline refused" bash "$F" --src "$R/gpsp" --out "$T/o2" --baseline --no-sidecars --flavours release
[ ! -s "$FAKE_DOCKER_LOG" ] && ok "M1 refused before any docker run" || bad "M1 a build ran on a dirty tree"
expect_rc 0 "M1' dirty tree without --baseline builds (recorded)" bash "$F" --src "$R/gpsp" --out "$T/o2b" --no-sidecars --flavours release
grep -q '"workingTreeClean": false' "$T/o2b/release/flavour.json" 2>/dev/null && ok "M1' workingTreeClean recorded false" || bad "M1' workingTreeClean not false"
# C2 (tree still dirty)
expect_rc 0 "C2 --commit HEAD from a dirty checkout" bash "$F" --commit HEAD --out "$T/o3" --baseline --no-sidecars --flavours release
if grep -q dirty "$T/o3/release/gpsp_adhoc.elf" 2>/dev/null; then bad "C2 the dirty edit was built"; else ok "C2 only the committed tree was built"; fi
[ ! -d "$T/o3/src-release" ] && [ ! -d "$T/o3/.work" ] && ok "C2 export and work dir removed after the build" || bad "C2 export left behind"
(cd "$R" && git checkout -q -- gpsp/marker.txt)
# M2
expect_rc 4 "M2 broken-imports warning with build.sh exit 0" env FAKE_FIXUP=1 bash "$F" --src "$R/gpsp" --out "$T/o4" --no-sidecars --flavours release
# M3
expect_rc 1 "M3 SOURCE_DATE_EPOCH dropped -> stamp check" env FAKE_NOSDE=1 bash "$F" --src "$R/gpsp" --out "$T/o5" --no-sidecars --flavours harness
grep -q "SOURCE_DATE_EPOCH did not reach" "$T/last.log" && ok "M3 message names the cause" || bad "M3 wrong failure"
# M4
expect_rc 1 "M4 failing build.sh" env FAKE_FAIL=1 bash "$F" --src "$R/gpsp" --out "$T/o6" --no-sidecars --flavours release
# M5
expect_rc 2 "M5 unknown flavour" bash "$F" --src "$R/gpsp" --out "$T/o7" --flavours "release bogus"

echo "test_flavours: $([ "$FAILS" -eq 0 ] && echo PASS || echo "$FAILS FAILED")"
exit $([ "$FAILS" -eq 0 ] && echo 0 || echo 1)
