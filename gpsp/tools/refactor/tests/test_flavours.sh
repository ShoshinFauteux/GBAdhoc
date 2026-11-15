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
#   M10 hermetic: a caller variable (ME_LOWLAT=0) never reaches build.sh, and is recorded
#   M11 the --against contract: clean tree, lock, extraEnv, buildEnv, hermetic, ancestry,
#       all four flavours: each clause refused with exit 2
#   (P-review 2026-11-08)
#   C6/M12 F4 --ref-ancestor: an ancestor reference passes the contract only when named, and
#       only if the name is the reference's commit and an ancestor (M12a-d refused, exit 2)
#   C7/M13 F5 the winpath cache: with WSL's git unable to read the repo (only git.exe can, as
#       on a Windows worktree) and wslpath failing after its first call, a --commit build
#       passes and calls wslpath once; the same script without the priming fails (exit 2)
#   (P7 2026-11-09)
#   C8/M14 F4 on a real --no-ff merge (PLAN-P-beta5 P7: P-tip merged with the beta tip,
#       proven against the beta tip): the merge's SECOND parent is accepted only when named
#       and is recorded as "parent 2" (M14a unnamed, M14b naming the first parent: exit 2);
#       its first parent still needs no name (C8')
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
# the caller's flavours.sh settings would point the fake repo's runs elsewhere
# (run_all with FLAV_REPO=<worktree> exported the worktree's HEAD in C2)
unset FLAV_REPO FLAV_WORK FLAV_LOCKED RFU_SCHED
for v in $(compgen -e | grep -E '^FLAV_[A-Z_]+_ENV$'); do unset "$v"; done
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
# image identity and the compiler version (flavours.sh records both)
case "$*" in
  "image inspect --format {{index .RepoDigests 0}} "*) echo "pspdev/pspdev@sha256:fakedigest"; exit 0 ;;
  "image inspect --format {{.Id}} "*) echo "${FAKE_IMAGE_ID:-sha256:fakeid}"; exit 0 ;;
  *"psp-gcc --version"*) echo "psp-gcc (GCC) 15.2.0"; exit 0 ;;
esac
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
# build.sh runs under env -i (HERMETIC): the fakes' controls are passed through on purpose
export FLAV_TEST_PASSENV="FAKE_DOCKER_LOG FAKE_STAMP FAKE_FAIL FAKE_FIXUP FAKE_NOSDE FAKE_IMAGE_ID"

# --- fake repo: gpsp/tools/build.sh + the real flavours scripts under test
R="$T/repo"; mkdir -p "$R/gpsp/tools/refactor/tests" "$R/gpsp/psp"
cp "$HERE/flavours.sh" "$HERE/flavours_sidecar.sh" "$HERE/flavours_gate.py" "$R/gpsp/tools/refactor/"
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
V="${GPSP_BUILD_VERSION:-${GPSP_BUILD_COMMIT:0:7}}"
{ printf 'ELF '; tr '|' '\n' < "$FAKE_STAMP"; printf '\n'; cat marker.txt; printf 'gpSP v1.1.0-%s rfu=%s lowlat=%s\n' "$V" "${RFU_SCHED:-0}" "${ME_LOWLAT:-unset}"; } > psp/gpsp_adhoc.elf
cp psp/gpsp_adhoc.elf psp/EBOOT.PBP
if [ "$1" = release ] && [ "${RFU_SCHED:-0}" = 1 ]; then echo "[build] layout  : layout.lock [release] OK"; LL=release:ok
elif [ "$1" = release ]; then echo "[build] layout  : variant arm, the lock is not enforced (RFU_SCHED=0)"; echo "[build] layout  : layout.lock [release] differs (variant arm: reported, not enforced)"; LL=release:variant
elif [ "$1" = lab ] && [ "${LAYOUT_PIN:-1}" = 1 ]; then echo "[build] layout  : layout.lock [lab] OK"; LL=lab:ok
else LL=none; fi
echo "{\"profile\":\"$1\",\"dirty\":\"${GPSP_BUILD_DIRTY:-unset}\", \"layoutLock\": \"$LL\"}" > psp/build-manifest.json
[ "${2:-}" = --out ] && mkdir -p "$3" && cp psp/build-manifest.json "$3/"
exit 0
EOF
echo committed > "$R/gpsp/marker.txt"; : > "$R/gpsp/psp/.keep"
mkdir -p "$R/gpsp/tools/layout"; echo "# fake lock: the fake build.sh reports release:ok / lab:ok" > "$R/gpsp/tools/layout/layout.lock"
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

# ---- P0a (PLAN-P-beta5 §3): pins, release-stable, --against contract ----
# C3 release-stable = build.sh release with RFU_SCHED=0; its layout line is recorded
expect_rc 0 "C3 release-stable builds" bash "$F" --src "$R/gpsp" --out "$T/o8" --no-sidecars --flavours "release release-stable"
grep -q 'rfu=0' "$T/o8/release-stable/gpsp_adhoc.elf" && grep -q 'rfu=1' "$T/o8/release/gpsp_adhoc.elf" \
  && ok "C3 release-stable built with RFU_SCHED=0, release with 1" || bad "C3 RFU_SCHED per flavour wrong"
grep -q '"buildShProfile": "release"' "$T/o8/release-stable/flavour.json" && ok "C3 release-stable uses build.sh release" || bad "C3 profile"
grep -q '"layout": "variant arm, the lock is not enforced (RFU_SCHED=0)' "$T/o8/release-stable/flavour.json" \
  && ok "C3 manifest records 'layout: variant arm, the lock is not enforced (RFU_SCHED=0)'" || bad "C3 layout line not recorded"
grep -q '"dockerId": "sha256:fakeid"' "$T/o8/release/flavour.json" && grep -q '"pspGcc": "psp-gcc (GCC) 15.2.0"' "$T/o8/release/flavour.json" \
  && ok "C3 image id and psp-gcc version recorded" || bad "C3 compiler identity not recorded"
# M6 --sde pins SOURCE_DATE_EPOCH (stamp follows it), --build-version pins the version string
expect_rc 0 "M6 --sde 1700000000 --build-version pin7" bash "$F" --src "$R/gpsp" --out "$T/o9" --no-sidecars --sde 1700000000 --build-version pin7
grep -q '"sourceDateEpoch": "1700000000"' "$T/o9/harness/flavour.json" && grep -q 'Nov 14 2023' "$T/o9/harness/gpsp_adhoc.elf" \
  && ok "M6 --sde reached the compiler (stamp Nov 14 2023)" || bad "M6 --sde not applied"
grep -q 'gpSP v1.1.0-pin7 ' "$T/o9/release/gpsp_adhoc.elf" && grep -q '"buildVersion": "pin7"' "$T/o9/release/flavour.json" \
  && ok "M6 --build-version reached build.sh (GPSP_BUILD_VERSION) and is recorded" || bad "M6 version pin not applied"
expect_rc 2 "M6' --sde not a number" bash "$F" --src "$R/gpsp" --out "$T/o9b" --sde yesterday
# a second commit with the same tree: unpinned it differs (hash), pinned it is identical
(cd "$R" && git -c user.email=t@t -c user.name=t commit -q --allow-empty -m same-tree)
expect_rc 0 "C4 same-tree commit, pinned" bash "$F" --src "$R/gpsp" --out "$T/o10" --no-sidecars --sde 1700000000 --build-version pin7
cmp -s "$T/o9/release/gpsp_adhoc.elf" "$T/o10/release/gpsp_adhoc.elf" && cmp -s "$T/o9/harness/gpsp_adhoc.elf" "$T/o10/harness/gpsp_adhoc.elf" \
  && ok "C4 two commits, one tree, pinned: identical ELFs" || bad "C4 pinned same-tree builds differ"
expect_rc 0 "C4' same-tree commit, unpinned" bash "$F" --src "$R/gpsp" --out "$T/o11" --no-sidecars --flavours release
cmp -s "$T/o9/release/gpsp_adhoc.elf" "$T/o11/release/gpsp_adhoc.elf" && bad "C4' unpinned build equals the pinned one" || ok "C4' without the pin the version string differs"
# M7 --against refuses (exit 2) a reference built with another image / version / SDE
cp -r "$T/o9" "$T/ref_img"; sed -i 's/"dockerId": "sha256:fakeid"/"dockerId": "sha256:doctored"/' "$T/ref_img/release/flavour.json"
expect_rc 2 "M7 --against with a doctored dockerId" bash "$F" --check-only --out "$T/o10" --against "$T/ref_img"
grep -q "dockerId differs" "$T/last.log" && ok "M7 message names dockerId" || bad "M7 wrong refusal"
cp -r "$T/o9" "$T/ref_ver"; sed -i 's/"buildVersion": "pin7"/"buildVersion": "abc1234"/' "$T/ref_ver/harness/flavour.json"
expect_rc 2 "M7' --against with another build version" bash "$F" --check-only --out "$T/o10" --against "$T/ref_ver"
expect_rc 2 "M7'' --against an unpinned build (other version + SDE)" bash "$F" --check-only --out "$T/o11" --against "$T/o9" --flavours release
# M8 --against in a tree whose checks are ABSENT: every check FAILs (never a note), gate.txt says so
expect_rc 6 "M8 absent tools are FAIL, not skipped" bash "$F" --check-only --out "$T/o10" --against "$T/o9"
if [ -f "$T/o10/gate.txt" ] && grep -q 'G1 FAIL' "$T/o10/gate.txt" && grep -q 'CI FAIL' "$T/o10/gate.txt"; then
  ok "M8 gate line generated from checks.txt: $(cat "$T/o10/gate.txt")"; else bad "M8 gate line: $(cat "$T/o10/gate.txt" 2>/dev/null)"; fi
# M9 sidecar: a \-continued compile line (layout_pad_gb.o, psp/Makefile) is joined before replay
SL="$T/eboot.log"
printf '%s\n' 'make: Entering directory '"'"'/build/psp'"'"'' \
  'psp-gcc -c -o layout_pad_gb.o layout_pad_mid.S -DLAYOUT_PAD_MID_BYTES=1040 \' \
  '      -DLAYOUT_PAD_MID_NAME=layout_pad_gb' \
  'psp-gcc -O2 -G0 -c -o main_psp.o main_psp.c' > "$SL"
E1=$(bash "$HERE/flavours_sidecar.sh" emit "$SL" /build/psp)
if printf '%s\n' "$E1" | grep -q -- '-o layout_pad_gb.o layout_pad_mid.S -DLAYOUT_PAD_MID_BYTES=1040 *-DLAYOUT_PAD_MID_NAME=layout_pad_gb$' \
   && ! printf '%s\n' "$E1" | grep -q '\\$' && [ "$(printf '%s\n' "$E1" | wc -l)" = 2 ]; then
  ok "M9 continued layout_pad_gb.o line joined (2 compiles, no trailing backslash)"; else bad "M9 emit: $E1"; fi
# the mutant: the sidecar without the join (the pre-P0a emit) is caught by the same check
sed 's/while (\$0 ~ \/\\\\\$\/ &&/while (0 \&\&/' "$HERE/flavours_sidecar.sh" > "$T/sidecar_nojoin.sh"
E2=$(bash "$T/sidecar_nojoin.sh" emit "$SL" /build/psp 2>/dev/null)
[ "$(printf '%s\n' "$E2" | wc -l)" = 2 ] || bad "M9 mutant emit is not the old 2-line output: $E2"
if cmp -s "$HERE/flavours_sidecar.sh" "$T/sidecar_nojoin.sh"; then bad "M9 mutant: the join line was not removed"
elif printf '%s\n' "$E2" | grep -q -- '-DLAYOUT_PAD_MID_NAME=layout_pad_gb$'; then bad "M9 mutant without the join still passed"
else ok "M9 mutant (no join) loses -DLAYOUT_PAD_MID_NAME: caught"; fi


# ---- adversarial review 2026-11-07: hermetic builds, the strict --against contract ----
# M10 a variable left in the caller's shell (ME_LOWLAT=0, MGIFT=0) does not reach build.sh
expect_rc 0 "M10 build with ME_LOWLAT=0 MGIFT=0 exported" env ME_LOWLAT=0 MGIFT=0 bash "$F" --src "$R/gpsp" --out "$T/o12" --no-sidecars --sde 1700000000 --build-version pin7
grep -q 'lowlat=unset' "$T/o12/release/gpsp_adhoc.elf" && ok "M10 build.sh saw no ME_LOWLAT (hermetic env -i)" || bad "M10 ME_LOWLAT leaked into build.sh"
grep -q '"callerEnvDropped": ".*ME_LOWLAT=0' "$T/o12/release/flavour.json" && grep -q '"hermetic": "yes"' "$T/o12/release/flavour.json" \
  && ok "M10 the dropped variable is recorded (callerEnvDropped)" || bad "M10 callerEnvDropped not recorded"
cmp -s "$T/o10/release/gpsp_adhoc.elf" "$T/o12/release/gpsp_adhoc.elf" && ok "M10 the leaky shell builds the same ELF" || bad "M10 ELF differs"
# M11 each clause of the contract refuses (exit 2) before any check
doct() {  # doct <name> <flavour> <sed expr>: a copy of o9 with one flavour.json doctored
  rm -rf "$T/d_$1"; cp -r "$T/o9" "$T/d_$1"; sed -i "$3" "$T/d_$1/$2/flavour.json"
}
doct clean lab 's/"workingTreeClean": true/"workingTreeClean": false/'
expect_rc 2 "M11a reference with workingTreeClean false" bash "$F" --check-only --out "$T/o10" --against "$T/d_clean"
grep -q "workingTreeClean false" "$T/last.log" && ok "M11a names workingTreeClean" || bad "M11a wrong refusal"
doct lock lab 's/"layoutLock": "lab:ok"/"layoutLock": "lab:differs"/'
expect_rc 2 "M11b reference whose lab lock is not ok" bash "$F" --check-only --out "$T/o10" --against "$T/d_lock"
grep -q "layoutLock is 'lab:differs'" "$T/last.log" && ok "M11b names the lock" || bad "M11b wrong refusal"
doct xenv harness 's/"extraEnv": ""/"extraEnv": "INPUT_FRESH=0"/'
expect_rc 2 "M11c reference with another extraEnv" bash "$F" --check-only --out "$T/o10" --against "$T/d_xenv"
grep -q "extraEnv differs" "$T/last.log" && ok "M11c names extraEnv" || bad "M11c wrong refusal"
doct benv release 's/"buildEnv": "/"buildEnv": "ME_LOWLAT=0 /'
expect_rc 2 "M11d reference with another buildEnv (ME_LOWLAT=0 passed)" bash "$F" --check-only --out "$T/o10" --against "$T/d_benv"
grep -q "buildEnv differs" "$T/last.log" && ok "M11d names buildEnv" || bad "M11d wrong refusal"
doct herm release-stable 's/"hermetic": "yes"/"hermetic": "no"/'
expect_rc 2 "M11e a non-hermetic reference" bash "$F" --check-only --out "$T/o10" --against "$T/d_herm"
rm -rf "$T/d_anc"; cp -r "$T/o9" "$T/d_anc"
for f in release release-stable lab harness; do sed -i 's/"sourceCommit": "[0-9a-f]*"/"sourceCommit": "0000000000000000000000000000000000000000"/; s/"gpspTree": "[0-9a-f]*"/"gpspTree": "1111111111111111111111111111111111111111"/' "$T/d_anc/$f/flavour.json"; done
expect_rc 2 "M11f a reference that is neither the commit, its parent nor its gpsp/ tree" bash "$F" --check-only --out "$T/o10" --against "$T/d_anc"
grep -q "nothing ties the step to its parent" "$T/last.log" && ok "M11f names the ancestry" || bad "M11f wrong refusal"
rm -rf "$T/d_miss"; cp -r "$T/o9" "$T/d_miss"; rm -rf "$T/d_miss/harness"
expect_rc 2 "M11g a reference without the harness flavour" bash "$F" --check-only --out "$T/o10" --against "$T/d_miss"
expect_rc 2 "M11h --against with --flavours release only" bash "$F" --check-only --out "$T/o10" --against "$T/o9" --flavours release
expect_rc 2 "M11i FLAV_LAB_ENV on this build only (extraEnv differs)" env FLAV_LAB_ENV="INPUT_FRESH=0" bash "$F" --src "$R/gpsp" --out "$T/o13" --no-sidecars --sde 1700000000 --build-version pin7 --against "$T/o9"
echo dirty >> "$R/gpsp/marker.txt"
expect_rc 2 "M11j a dirty --src (no --baseline) gated --against" bash "$F" --src "$R/gpsp" --out "$T/o14" --no-sidecars --sde 1700000000 --build-version pin7 --against "$T/o9"
(cd "$R" && git checkout -q -- gpsp/marker.txt)

# ---- P-review 2026-11-08: F4 --ref-ancestor, F5 the winpath cache ----
# C6 / M12 (F4): a third commit with another gpsp/ tree; o9 (two commits back) is its ancestor
echo tree2 > "$R/gpsp/marker.txt"
(cd "$R" && git add -A && git -c user.email=t@t -c user.name=t commit -q -m tree2)
expect_rc 0 "C6 a third commit (another gpsp/ tree), pinned" bash "$F" --src "$R/gpsp" --out "$T/o15" --no-sidecars --sde 1700000000 --build-version pin7
BASE_C=$(sed -nE 's/.*"sourceCommit": *"([0-9a-f]+)".*/\1/p' "$T/o9/release/flavour.json")
MID_C=$(cd "$R" && git rev-parse HEAD~1)
expect_rc 2 "M12a an ancestor reference (two commits back, another tree), not named" bash "$F" --check-only --out "$T/o15" --against "$T/o9"
grep -q "nothing ties the step to its parent" "$T/last.log" && ok "M12a names the ancestry" || bad "M12a wrong refusal"
expect_rc 6 "C6 the same with --ref-ancestor <its commit>: contract ok (exit 6 = this fake repo's absent tools)" \
  bash "$F" --check-only --out "$T/o15" --against "$T/o9" --ref-ancestor "$BASE_C"
grep -q "contract ok (reference = named ancestor ${BASE_C:0:10}, 2 commit(s) before" "$T/last.log" \
  && grep -q -- "-- contract: reference = named ancestor ${BASE_C:0:10}" "$T/o15/checks.txt" \
  && ok "C6 the named ancestor is the recorded ancestry (log and checks.txt)" || bad "C6 ancestry not recorded"
expect_rc 2 "M12b --ref-ancestor naming another commit than the reference's" bash "$F" --check-only --out "$T/o15" --against "$T/o9" --ref-ancestor "$MID_C"
grep -q "is not the reference's commit" "$T/last.log" && ok "M12b names the mismatch" || bad "M12b wrong refusal"
SIDE_C=$(cd "$R" && git -c user.email=t@t -c user.name=t commit-tree "$BASE_C^{tree}" -p "$BASE_C" -m side)
rm -rf "$T/d_side"; cp -r "$T/o9" "$T/d_side"
for f in release release-stable lab harness; do sed -i "s/\"sourceCommit\": \"[0-9a-f]*\"/\"sourceCommit\": \"$SIDE_C\"/" "$T/d_side/$f/flavour.json"; done
expect_rc 2 "M12c --ref-ancestor naming a side commit that is not an ancestor" bash "$F" --check-only --out "$T/o15" --against "$T/d_side" --ref-ancestor "$SIDE_C"
grep -q "is not an ancestor of this build" "$T/last.log" && ok "M12c names it" || bad "M12c wrong refusal"
expect_rc 2 "M12d --ref-ancestor without --against" bash "$F" --src "$R/gpsp" --out "$T/o16" --no-sidecars --ref-ancestor "$BASE_C"
grep -q -- "--ref-ancestor needs --against" "$T/last.log" && ok "M12d usage" || bad "M12d wrong refusal"
# C7 / M13 (F5): WSL's git cannot read the repo (only git.exe can), wslpath works once then fails
REALGIT=$(command -v git); mkdir -p "$T/wsl"
cat > "$T/wsl/git" <<EOF
#!/bin/sh
# fake: WSL's own git cannot read the repo (a Windows worktree's .git file); git.exe can
if [ "\$1" = -C ]; then case "\$2" in "$R"|"$R"/*) echo "fatal: not a git repository" >&2; exit 128 ;; esac; fi
exec "$REALGIT" "\$@"
EOF
printf '#!/bin/sh\nexec "%s" "$@"\n' "$REALGIT" > "$T/wsl/git.exe"
cat > "$T/wsl/wslpath" <<'EOF'
#!/bin/sh
# fake: -w DIR prints DIR; every call after the first $FAKE_WSLPATH_OK fails (a transient fault)
n=$(cat "$FAKE_WSLPATH_N" 2>/dev/null || echo 0); n=$((n + 1)); echo "$n" > "$FAKE_WSLPATH_N"
[ "$1" = -w ] || exit 1
[ "$n" -le "${FAKE_WSLPATH_OK:-99}" ] || { echo "wslpath: transient fault" >&2; exit 1; }
printf '%s\n' "$2"
EOF
chmod +x "$T/wsl/git" "$T/wsl/git.exe" "$T/wsl/wslpath"
rm -f "$T/wslpath.n"
expect_rc 0 "C7 --commit build through git.exe with wslpath failing after its first call" \
  env PATH="$T/wsl:$PATH" FAKE_WSLPATH_N="$T/wslpath.n" FAKE_WSLPATH_OK=1 bash "$F" --commit HEAD --out "$T/o17" --no-sidecars --flavours release
[ "$(cat "$T/wslpath.n" 2>/dev/null)" = 1 ] && ok "C7 wslpath called once (the cache is filled in flavours.sh's own shell)" \
  || bad "C7 wslpath called $(cat "$T/wslpath.n" 2>/dev/null) times"
sed -e '/^winpath_prime "\$REPO"/d' -e '/^  winpath_prime "\$SRC"$/d' "$F" > "$R/gpsp/tools/refactor/flavours_noprime.sh"
if cmp -s "$F" "$R/gpsp/tools/refactor/flavours_noprime.sh"; then bad "M13 mutant (no priming) not applied"
else
  rm -f "$T/wslpath.n"
  expect_rc 2 "M13 the same without the priming (the pre-review cache in a subshell) fails" \
    env PATH="$T/wsl:$PATH" FAKE_WSLPATH_N="$T/wslpath.n" FAKE_WSLPATH_OK=1 bash "$R/gpsp/tools/refactor/flavours_noprime.sh" --commit HEAD --out "$T/o18" --no-sidecars --flavours release
  grep -q "wslpath -w .* failed" "$T/last.log" && ok "M13 the failure names wslpath (it used to be a bare 'no such commit')" || bad "M13 message: $(tail -2 "$T/last.log")"
fi
rm -f "$R/gpsp/tools/refactor/flavours_noprime.sh"
# C8 / M14 (F4 on a REAL merge, PLAN-P-beta5 P7: the P branch merges the beta tip and is
# proven against it, the merge's SECOND parent).  side2 (off the base, another gpsp/ tree)
# is merged into tree2 with --no-ff; the reference is built from side2.
T2_C=$(cd "$R" && git rev-parse HEAD)
(cd "$R" && git checkout -q -b side2 "$BASE_C" && echo side2 > gpsp/side2.txt && git add -A \
  && git -c user.email=t@t -c user.name=t commit -q -m side2 && git checkout -q - \
  && git -c user.email=t@t -c user.name=t merge -q --no-ff -m merge side2) || bad "C8 could not make the merge"
SIDE2_C=$(cd "$R" && git rev-parse side2); MERGE_C=$(cd "$R" && git rev-parse HEAD)
[ "$(cd "$R" && git rev-parse HEAD^1 HEAD^2 | tr '\n' ' ')" = "$T2_C $SIDE2_C " ] \
  && ok "C8 the merge's parents are tree2 (1st) and side2 (2nd)" || bad "C8 merge parents"
expect_rc 0 "C8 the merge's second parent (side2), pinned" bash "$F" --commit "$SIDE2_C" --out "$T/o19" --no-sidecars --sde 1700000000 --build-version pin7
expect_rc 0 "C8 the merge, pinned" bash "$F" --commit "$MERGE_C" --out "$T/o20" --no-sidecars --sde 1700000000 --build-version pin7
expect_rc 2 "M14a the merge against its second parent, not named" bash "$F" --check-only --out "$T/o20" --against "$T/o19"
grep -q "nothing ties the step to its parent" "$T/last.log" && ok "M14a names the ancestry" || bad "M14a wrong refusal"
expect_rc 6 "C8 the merge --against its second parent --ref-ancestor <it>: contract ok (exit 6 = absent tools)" \
  bash "$F" --check-only --out "$T/o20" --against "$T/o19" --ref-ancestor "$SIDE2_C"
grep -qF "contract ok (reference = named ancestor ${SIDE2_C:0:10}, parent 2 of ${MERGE_C:0:10} (--ref-ancestor)" "$T/last.log" \
  && grep -qF -- "-- contract: reference = named ancestor ${SIDE2_C:0:10}, parent 2 of ${MERGE_C:0:10}" "$T/o20/checks.txt" \
  && ok "C8 recorded as the merge's parent 2 (log and checks.txt)" || bad "C8 parent 2 not recorded: $(grep contract "$T/last.log")"
expect_rc 2 "M14b --ref-ancestor naming the merge's FIRST parent while the reference is the second" \
  bash "$F" --check-only --out "$T/o20" --against "$T/o19" --ref-ancestor "$T2_C"
grep -q "is not the reference's commit" "$T/last.log" && ok "M14b names the mismatch" || bad "M14b wrong refusal"
expect_rc 6 "C8' the merge against its first parent (tree2, o15), unnamed: contract ok" \
  bash "$F" --check-only --out "$T/o20" --against "$T/o15"
grep -qF "contract ok (reference = first parent ${T2_C:0:10}" "$T/last.log" && ok "C8' the first parent needs no name" || bad "C8' first parent"

# C5 (optional, needs a real build pair): --check-only with the REAL tools on two
# runs of one commit, e.g. FLAV_REAL_RUNS=$HOME/p0a/fbe/baseline (run1, run2)
if [ -n "${FLAV_REAL_RUNS:-}" ] && [ -d "$FLAV_REAL_RUNS/run2" ]; then
  expect_rc 0 "C5 real tools: --check-only run2 --against run1" bash "$HERE/flavours.sh" --check-only \
    --out "$FLAV_REAL_RUNS/run2" --against "$FLAV_REAL_RUNS/run1"
  G=$(cat "$FLAV_REAL_RUNS/run2/gate.txt" 2>/dev/null)
  for g in G0 G0var G1 G2 HO SS ST LL CF BF DS CI; do
    case " $G " in *" $g ok"*) ;; *) bad "C5 gate $g missing or red: $G"; continue 2 ;; esac
  done
  ok "C5 every check present and green: $G"
else
  echo "  (C5 real-tools --against skipped: set FLAV_REAL_RUNS=<dir with run1/ run2/>)"
fi

echo "test_flavours: $([ "$FAILS" -eq 0 ] && echo PASS || echo "$FAILS FAILED")"
exit $([ "$FAILS" -eq 0 ] && echo 0 || echo 1)
