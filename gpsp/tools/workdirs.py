"""workdirs.py -- the Python side of tools/workdirs.sh: where the LOCAL, untracked inputs and
outputs live.  tools/README.md ("Local folders") documents the variables.

    from workdirs import repo, work, roms     (tools/ on sys.path)

repo()  GBADHOC_REPO, default the checkout this file is in (two levels above gpsp/tools)
work()  GBADHOC_WORK, default the first `builds` folder at or above repo()
roms()  GBADHOC_ROMS, default repo()/roms; may be a list (os.pathsep-separated): roms_list()
"""
import os

_HERE = os.path.dirname(os.path.abspath(__file__))


def repo():
    return os.environ.get('GBADHOC_REPO') or os.path.normpath(os.path.join(_HERE, '..', '..'))


def work():
    w = os.environ.get('GBADHOC_WORK')
    if w:
        return w
    d = repo()
    while not os.path.isdir(os.path.join(d, 'builds')):
        up = os.path.dirname(d)
        if up == d:
            break
        d = up
    return os.path.join(d, 'builds')


def roms():
    return os.environ.get('GBADHOC_ROMS') or os.path.join(repo(), 'roms')


def roms_list():
    return [p for p in roms().split(os.pathsep) if p]
