# workdirs.sh -- sourced by tool scripts: where the LOCAL, untracked inputs and outputs live.
# tools/README.md ("Local folders") documents the variables; set one to override its default.
#
#   GBADHOC_REPO  the repository root (default: the checkout this file is in, two levels above
#                 gpsp/tools).
#   GBADHOC_WORK  build outputs, rig stages and fixtures that earlier runs made (EBOOT
#                 directories, PPSSPP fixtures, ...).  Never in git.  Default: the first
#                 `builds` folder at or above GBADHOC_REPO, so the main checkout uses its own
#                 builds/ and a worktree kept inside that folder finds the same one.
#   GBADHOC_ROMS  the ROM library (never in git).  Default: GBADHOC_REPO/roms.
#
# A script that needs a folder that does not exist says so and stops; nothing here creates one.
_wd_here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GBADHOC_REPO=${GBADHOC_REPO:-$(cd "$_wd_here/../.." && pwd)}
if [ -z "${GBADHOC_WORK:-}" ]; then
  _wd_d=$GBADHOC_REPO
  while [ ! -d "$_wd_d/builds" ] && [ "$_wd_d" != / ]; do _wd_d=$(dirname "$_wd_d"); done
  GBADHOC_WORK=$_wd_d/builds
fi
GBADHOC_ROMS=${GBADHOC_ROMS:-$GBADHOC_REPO/roms}
export GBADHOC_REPO GBADHOC_WORK GBADHOC_ROMS
unset _wd_here _wd_d
