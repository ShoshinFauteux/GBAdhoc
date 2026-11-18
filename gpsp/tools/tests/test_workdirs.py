#!/usr/bin/env python3
"""test_workdirs.py -- tools/workdirs.sh and tools/workdirs.py pick the same GBADHOC_WORK, and it
is the local work folder, never a checkout's own (tracked) builds/.

Builds a throwaway layout in a temp dir:

    T/lab/builds/             the work folder (untracked outputs live here)
    T/lab/builds/main/        the main checkout; builds/harness/ tracked, like the real repo
    T/lab/builds/wt/          a worktree inside the work folder   (the bug: it got wt/builds)
    T/far/wt2/                a worktree elsewhere                 (found through git's common dir)
    T/lab/builds/export/      a copy with no .git (a git archive export, or WSL's git unable to
                              read a worktree's Windows-path .git file)

and checks each, both implementations, plus "GBADHOC_WORK set explicitly wins".
Needs git and bash.  Exit 0 pass, 1 fail.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAILS = []


def git(*args, cwd=None):
    subprocess.run(['git', '-c', 'user.name=t', '-c', 'user.email=t@example.invalid',
                    '-c', 'init.defaultBranch=main', *args],
                   cwd=cwd, check=True, capture_output=True, text=True)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('GBADHOC_', 'GIT_'))}
    env.update(extra)
    return env


def populate(checkout):
    os.makedirs(os.path.join(checkout, 'gpsp', 'tools'), exist_ok=True)
    os.makedirs(os.path.join(checkout, 'builds', 'harness'), exist_ok=True)
    for f in ('workdirs.sh', 'workdirs.py'):
        shutil.copy(os.path.join(TOOLS, f), os.path.join(checkout, 'gpsp', 'tools', f))
    with open(os.path.join(checkout, 'builds', 'harness', 'tracked.ini'), 'w') as fh:
        fh.write('tracked\n')


def sh_work(checkout, **env):
    src = os.path.join(checkout, 'gpsp', 'tools', 'workdirs.sh')
    r = subprocess.run(['bash', '-c', 'set -eu; source "$1"; printf %s "$GBADHOC_WORK"', 'x', src],
                       env=clean_env(**env), capture_output=True, text=True, cwd=checkout)
    if r.returncode:
        return 'ERROR rc %d: %s' % (r.returncode, r.stderr.strip())
    return r.stdout


def py_work(checkout, **env):
    src = os.path.join(checkout, 'gpsp', 'tools', 'workdirs.py')
    prog = ('import importlib.util,sys\n'
            's=importlib.util.spec_from_file_location("wd",sys.argv[1])\n'
            'm=importlib.util.module_from_spec(s); s.loader.exec_module(m)\n'
            'sys.stdout.write(m.work())\n')
    r = subprocess.run([sys.executable, '-I', '-c', prog, src],
                       env=clean_env(**env), capture_output=True, text=True, cwd=checkout)
    if r.returncode:
        return 'ERROR rc %d: %s' % (r.returncode, r.stderr.strip())
    return r.stdout


def same(a, b):
    try:
        return os.path.samefile(a, b)
    except OSError:
        return os.path.normpath(a) == os.path.normpath(b)


def expect(case, checkout, want, **env):
    for impl, fn in (('sh', sh_work), ('py', py_work)):
        got = fn(checkout, **env)
        ok = not got.startswith('ERROR') and same(got, want)
        print('%-4s %-34s %-3s -> %s' % ('ok' if ok else 'FAIL', case, impl, got))
        if not ok:
            FAILS.append('%s (%s): got %s, want %s' % (case, impl, got, want))
        if got.startswith('ERROR') is False and same(got, os.path.join(checkout, 'builds')) \
                and not same(want, os.path.join(checkout, 'builds')):
            FAILS.append('%s (%s): resolved to the checkout\'s own tracked builds/' % (case, impl))


def main():
    for tool in ('git', 'bash'):
        if not shutil.which(tool):
            print('test_workdirs: %s not found' % tool)
            return 1
    t = tempfile.mkdtemp(prefix='workdirs-test-')
    try:
        work = os.path.join(t, 'lab', 'builds')
        main_co = os.path.join(work, 'main')
        os.makedirs(main_co)
        populate(main_co)
        git('init', '-q', cwd=main_co)
        git('add', '-A', cwd=main_co)
        git('commit', '-q', '-m', 'init', cwd=main_co)
        wt = os.path.join(work, 'wt')
        git('worktree', 'add', '-q', '-b', 'wt', wt, cwd=main_co)
        far = os.path.join(t, 'far', 'wt2')
        os.makedirs(os.path.dirname(far))
        git('worktree', 'add', '-q', '-b', 'wt2', far, cwd=main_co)
        export = os.path.join(work, 'export')
        populate(export)

        assert os.path.isdir(os.path.join(wt, 'builds', 'harness')), 'worktree lacks the tracked builds/'
        expect('main checkout', main_co, work)
        expect('worktree inside the work folder', wt, work)
        expect('worktree elsewhere', far, work)
        expect('export (no git)', export, work)
        override = os.path.join(t, 'mine')
        expect('GBADHOC_WORK set wins (worktree)', wt, override, GBADHOC_WORK=override)
        expect('GBADHOC_WORK set wins (export)', export, override, GBADHOC_WORK=override)
    finally:
        shutil.rmtree(t, ignore_errors=True)
    if FAILS:
        print('test_workdirs: FAIL')
        for f in FAILS:
            print('  ' + f)
        return 1
    print('test_workdirs: PASS (6 cases x sh/py)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
