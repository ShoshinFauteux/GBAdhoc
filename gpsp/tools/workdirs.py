"""workdirs.py -- the Python side of tools/workdirs.sh: where the LOCAL, untracked inputs and
outputs live.  tools/README.md ("Local folders") documents the variables.

    from workdirs import repo, work, roms     (tools/ on sys.path)

repo()  GBADHOC_REPO, default the checkout this file is in (two levels above gpsp/tools)
work()  GBADHOC_WORK, default the first `builds` folder ABOVE the main checkout (git's common
        dir names it), else above repo(), else repo()/builds.  So not a checkout's own
        builds/: it is tracked, so every worktree has one (workdirs.sh says more).
roms()  GBADHOC_ROMS, default repo()/roms; may be a list (os.pathsep-separated): roms_list()
"""
import os
import subprocess

_HERE = os.path.dirname(os.path.abspath(__file__))


def repo():
    return os.environ.get('GBADHOC_REPO') or os.path.normpath(os.path.join(_HERE, '..', '..'))


def main_checkout(start=None):
    """The main checkout of the repository at start (default repo()): the parent of git's
    common dir when that is a .git folder; None when git cannot say (no git, an export, a
    worktree whose .git file this git cannot read)."""
    start = start or repo()
    try:
        c = subprocess.run(['git', '-C', start, 'rev-parse', '--git-common-dir'],
                           capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    if not c:
        return None
    c = os.path.normpath(c if os.path.isabs(c) else os.path.join(start, c))
    if os.path.basename(c) != '.git' or not os.path.isdir(c):
        return None
    return os.path.dirname(c)


def _builds_above(path):
    d = os.path.dirname(os.path.normpath(path))
    while True:
        if os.path.isdir(os.path.join(d, 'builds')):
            return os.path.join(d, 'builds')
        up = os.path.dirname(d)
        if up == d:
            return None
        d = up


def work():
    w = os.environ.get('GBADHOC_WORK')
    if w:
        return w
    for s in (main_checkout(), repo()):
        if s:
            b = _builds_above(s)
            if b:
                return b
    return os.path.join(repo(), 'builds')


def roms():
    return os.environ.get('GBADHOC_ROMS') or os.path.join(repo(), 'roms')


def roms_list():
    return [p for p in roms().split(os.pathsep) if p]
