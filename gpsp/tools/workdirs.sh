# workdirs.sh -- sourced by tool scripts: where the LOCAL, untracked inputs and outputs live.
# tools/README.md ("Local folders") documents the variables; set one to override its default.
#
#   GBADHOC_REPO  the repository root (default: the checkout this file is in, two levels above
#                 gpsp/tools).
#   GBADHOC_WORK  build outputs, rig stages and fixtures that earlier runs made (EBOOT
#                 directories, PPSSPP fixtures, ...).  Never in git.  Default: the first
#                 `builds` folder ABOVE the main checkout (git's common dir names it), else
#                 above GBADHOC_REPO, else GBADHOC_REPO/builds.  So not a checkout's own
#                 builds/: that one is tracked (builds/harness, builds/playable), so every
#                 worktree has one, and stopping there sent a worktree to its own copy.
#   GBADHOC_ROMS  the ROM library (never in git).  Default: GBADHOC_REPO/roms.
#
# A script that needs a folder that does not exist says so and stops; nothing here creates one.
# tools/workdirs.py computes the same defaults; tools/tests/test_workdirs.py checks both.
_wd_here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GBADHOC_REPO=${GBADHOC_REPO:-$(cd "$_wd_here/../.." && pwd)}
if [ -z "${GBADHOC_WORK:-}" ]; then
  # the main checkout: the parent of git's common dir when that is a .git folder (in a
  # worktree it is the main checkout's); none when git cannot say (no git, an export, or a
  # worktree whose .git file holds a Windows path that WSL's git cannot read)
  _wd_main=
  _wd_c=$(git -C "$GBADHOC_REPO" rev-parse --git-common-dir 2>/dev/null) || _wd_c=
  case $_wd_c in
    '') ;;
    /*) ;;
    [A-Za-z]:[/\\]*) _wd_c=$(cygpath -u "$_wd_c" 2>/dev/null || wslpath -u "$_wd_c" 2>/dev/null) || _wd_c= ;;
    *) _wd_c=$GBADHOC_REPO/$_wd_c ;;
  esac
  if [ -n "$_wd_c" ] && [ "$(basename "$_wd_c")" = .git ] && [ -d "$_wd_c" ]; then
    _wd_main=$(cd "$_wd_c/.." && pwd)
  fi
  for _wd_s in "$_wd_main" "$GBADHOC_REPO"; do
    [ -n "$_wd_s" ] || continue
    _wd_d=$(dirname "$_wd_s")
    while [ ! -d "$_wd_d/builds" ] && [ "$_wd_d" != / ] && [ "$_wd_d" != . ]; do _wd_d=$(dirname "$_wd_d"); done
    if [ -d "$_wd_d/builds" ]; then GBADHOC_WORK=$_wd_d/builds; break; fi
  done
  GBADHOC_WORK=${GBADHOC_WORK:-$GBADHOC_REPO/builds}
fi
GBADHOC_ROMS=${GBADHOC_ROMS:-$GBADHOC_REPO/roms}
export GBADHOC_REPO GBADHOC_WORK GBADHOC_ROMS
unset _wd_here _wd_main _wd_c _wd_s _wd_d
