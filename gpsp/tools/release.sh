#!/usr/bin/env bash
# release.sh X.Y.Z -- make a release.  This one command, on a clean checkout of
# the commit to tag, with no environment (docs/RELEASE.md section 2).
#
#   tools/check_flags.py --compile   the flags are the committed ones
#   tools/build.sh release           the one clean build
#   tools/make_release.sh            refuses anything but that build; packages it
#
# It takes no options on purpose.  A variant is not a release.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 9
die() { echo "release: $*" >&2; exit 1; }
[ $# -eq 1 ] || die "usage: tools/release.sh X.Y.Z"
VERSION="$1"
echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9]+)*$' || die "'$VERSION' is not X.Y.Z"
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python

# What a release must not inherit.  GPSP_BUILD_VERSION is a reproduction aid:
# a release stamps its own commit unless RELEASE_ALLOW_VERSION_PIN=1 says that
# reproducing an older stamp is the point.
for v in CORE_EXTRA FE_EXTRA PSP_EXTRA GBA_LINK REWIND LAYOUT_PIN LAYOUT_PAD_BYTES CHEATS INPUT_FRESH \
         ME_LOWLAT GPSP_EBOOT_TITLE; do
  [ -z "${!v+x}" ] || die "$v is set in the environment; a release has none"
done
if [ -n "${GPSP_BUILD_VERSION:-}" ] && [ "${RELEASE_ALLOW_VERSION_PIN:-0}" != 1 ]; then
  die "GPSP_BUILD_VERSION is set; a release stamps its own commit"
fi
[ -z "$(git status --porcelain -- . 2>/dev/null)" ] || die "the tree is not clean: git status"

OUT=dist/release-build
"$PY" tools/check_flags.py --compile || die "flags drifted"
bash tools/build.sh release --out "$OUT" || die "build failed"
"$PY" tools/check_flags.py --build "$OUT" || die "the build is not the release set"
bash tools/make_release.sh "$OUT/EBOOT.PBP" --version "$VERSION" || die "packaging refused"

echo "release: $VERSION  EBOOT md5 $(md5sum < "$OUT/EBOOT.PBP" | cut -c1-32)"
echo "release: now boot it once (tools/e2e/run_boot_test.sh) and follow docs/RELEASE.md from the stranger's test."
