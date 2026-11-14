#!/usr/bin/env python3
"""aec_pspcc.py -- shared helper of the abi-exit-ci tools (T8 exit_matrix.py, T14 abi_asserts.py).

Runs psp-gcc in the pspdev/pspdev docker image with the EXACT compile line a flavour's
build used, taken from that flavour's make log (flavours.sh keeps them in
<flavour dir>/logs/.build-eboot.log for psp/*.c and .build-core.log for the core).
Nothing is reconstructed by hand, so a flag the build adds is a flag we see.

The source tree is mounted READ-ONLY; output goes to stdout or to a scratch dir that
is mounted separately.  This never builds an ELF: -E and -fsyntax-only only.

API:
  compile_argv(flavour_dir, obj, log)      -> argv list of the logged compile of `obj`
  pp_argv(argv, keep_markers=True)         -> the same line as `-E` (drops -c/-o/-MMD/-MP)
  docker_run(src_root, workdir, cmd, extra_mounts=None, image=...) -> (rc, stdout, stderr)
  preprocess(src_root, flavour_dir, obj='main_psp.o', subdir='psp') -> text of `-E`
"""
import os
import shlex
import subprocess
import sys

IMAGE = os.environ.get('AEC_DOCKER_IMAGE', 'pspdev/pspdev')


def compile_argv(flavour_dir, obj='main_psp.o', log='.build-eboot.log'):
    p = os.path.join(flavour_dir, 'logs', log)
    with open(p, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if not line.startswith('psp-gcc') or ' -c ' not in ' ' + line + ' ':
                continue
            try:
                argv = shlex.split(line)
            except ValueError:
                # a make recipe continued with `\` (psp/Makefile's layout_pad_*.o rules) logs
                # its first line ending in a lone backslash; it is never the object asked for
                if (' -o %s ' % obj) in line + ' ':
                    raise
                continue
            if '-o' in argv and argv[argv.index('-o') + 1] == obj:
                return argv
    raise SystemExit('aec_pspcc: no compile line for %s in %s' % (obj, p))


def pp_argv(argv, keep_markers=True):
    out, skip = [], False
    for a in argv:
        if skip:
            skip = False
            continue
        if a == '-o':
            skip = True
            continue
        if a in ('-c', '-MMD', '-MP', '-g'):
            continue
        out.append(a)
    src = out.pop()                     # the source file is last on every logged line
    out += ['-E'] + ([] if keep_markers else ['-P']) + [src]
    return out


def to_posix(path):
    """Windows path -> /mnt/c/... when we run under WSL; posix paths pass through."""
    path = os.path.abspath(path)
    if len(path) > 2 and path[1] == ':':
        return '/mnt/' + path[0].lower() + path[2:].replace('\\', '/')
    return path


def docker_run(src_root, workdir, cmd, extra_mounts=None, image=IMAGE):
    args = ['docker', 'run', '--rm', '-v', '%s:/build:ro' % to_posix(src_root)]
    for host, guest, mode in (extra_mounts or []):
        args += ['-v', '%s:%s:%s' % (to_posix(host), guest, mode)]
    args += ['-w', workdir, image, 'sh', '-c', cmd]
    p = subprocess.run(args, capture_output=True, text=True, errors='replace')
    return p.returncode, p.stdout, p.stderr


def preprocess(src_root, flavour_dir, obj='main_psp.o', subdir='psp', log='.build-eboot.log',
               override=None, extra=None):
    """override: a host file bind-mounted (read-only) over /build/<subdir>/<source>,
    so a MUTATED copy of one source is preprocessed without touching the tree.
    extra: [(host file, path relative to /build/<subdir>)], more read-only overlays (a
    mutated fragment fe/X.inc.c of a split main_psp.c)."""
    argv = pp_argv(compile_argv(flavour_dir, obj, log))
    mounts = [(override, '/build/%s/%s' % (subdir, argv[-1]), 'ro')] if override else []
    mounts += [(h, '/build/%s/%s' % (subdir, rel), 'ro') for h, rel in (extra or [])]
    mounts = mounts or None
    rc, out, err = docker_run(src_root, '/build/' + subdir, ' '.join(shlex.quote(a) for a in argv), mounts)
    if rc != 0:
        sys.stderr.write(err[-4000:])
        raise SystemExit('aec_pspcc: psp-gcc -E failed (rc %d)' % rc)
    return out


def macros(src_root, flavour_dir, obj='main_psp.o', subdir='psp', log='.build-eboot.log',
           override=None, extra=None):
    """{name: replacement} of every macro defined at the end of `obj`'s TU (-dM -E).
    override / extra: as preprocess()."""
    argv = pp_argv(compile_argv(flavour_dir, obj, log))
    argv.insert(len(argv) - 2, '-dM')
    mounts = [(override, '/build/%s/%s' % (subdir, argv[-1]), 'ro')] if override else []
    mounts += [(h, '/build/%s/%s' % (subdir, rel), 'ro') for h, rel in (extra or [])]
    mounts = mounts or None
    rc, out, err = docker_run(src_root, '/build/' + subdir, ' '.join(shlex.quote(a) for a in argv), mounts)
    if rc != 0:
        sys.stderr.write(err[-4000:])
        raise SystemExit('aec_pspcc: psp-gcc -dM -E failed (rc %d)' % rc)
    return out


def parse_dm(text):
    res = {}
    for line in text.splitlines():
        if line.startswith('#define '):
            rest = line[8:]
            i = 0
            while i < len(rest) and (rest[i].isalnum() or rest[i] == '_'):
                i += 1
            res[rest[:i]] = rest[i:].strip()
    return res


if __name__ == '__main__':
    # aec_pspcc.py <src_root> <flavour_dir> <out.i>  -- write main_psp.c's -E output
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    txt = preprocess(sys.argv[1], sys.argv[2])
    with open(sys.argv[3], 'w', encoding='utf-8', newline='\n') as f:
        f.write(txt)
    print('wrote %s (%d lines)' % (sys.argv[3], txt.count('\n')))
