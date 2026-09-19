#!/usr/bin/env python3
"""Native-GCC checks of actual config I/O and baseline/current ROM loading."""
from pathlib import Path
import subprocess, tempfile

repo = Path(__file__).resolve().parents[1]
baseline = 'ecdd8bcfa7396a41523bcf3d4230d3f1ee99da7f'

def old(name):
    """The baseline copy of `name`.

    Prefer a directory of pre-extracted files (GBADHOC_BASELINE_DIR, set by
    tools/run_host_tests.py).  Shelling out to git works in an ordinary clone
    but NOT in a git worktree reached from WSL: a worktree's .git file holds a
    Windows path that git inside WSL cannot resolve, so `git show` exits 128
    and this suite looked broken when only its environment was.
    """
    import os
    from pathlib import Path as _P
    d = os.environ.get('GBADHOC_BASELINE_DIR')
    if d:
        f = _P(d) / name
        if f.exists():
            return f.read_text()
        raise SystemExit('baseline dir %s has no %s' % (d, name))
    try:
        return subprocess.check_output(['git', 'show', baseline + ':' + name],
                                       cwd=repo).decode()
    except (subprocess.CalledProcessError, OSError) as e:
        raise SystemExit(
            'cannot read baseline %s:%s (%s). '
            'Run this through tools/run_host_tests.py, which extracts the '
            'baseline with a git that can see the repository.'
            % (baseline[:12], name, e))

def raw(source, suffix):
    start = source.index('static s32 load_gamepak_raw(')
    end = source.index('static bool rom_has_signature(', start)
    return source[start:end].replace('load_gamepak_raw(', 'load_gamepak_raw_' + suffix + '(')

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    before = tmp/'before'
    after = tmp/'after'
    before.mkdir()
    after.mkdir()
    common = ['gcc', '-std=gnu99', '-O2', '-Wall', '-Wextra',
              '-ffunction-sections', '-fdata-sections', '-DGPSP_PLAYABLE',
              '-I'+str(repo/'frontend-common'), '-I'+str(repo/'psp'), '-I'+str(tmp)]
    test_source = (repo/'tools/test_rom_selection.c').read_text().replace(
        '#include "../psp/config_psp.h"', '#include "config_psp.h"')
    (before/'config_psp.c').write_text(old('psp/config_psp.c'))
    (before/'config_psp.h').write_text(old('psp/config_psp.h'))
    (before/'test_rom_selection.c').write_text(test_source)
    (after/'test_rom_selection.c').write_text(test_source)
    for label, test, config, flags in [
            ('before', before/'test_rom_selection.c', before/'config_psp.c', ['-DBASELINE']),
            ('after', after/'test_rom_selection.c', repo/'psp/config_psp.c', [])]:
        binary=tmp/(label + '_test')
        subprocess.run(common + flags + [str(test), str(config),
                       str(repo/'frontend-common/fe_util.c'), '-Wl,--wrap=fopen',
                       '-Wl,--gc-sections', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    (tmp/'load_before.inc').write_text(raw(old('gba_memory.c'), 'before'))
    (tmp/'load_after.inc').write_text(raw((repo/'gba_memory.c').read_text(), 'after'))
    binary=tmp/'loader'
    subprocess.run(common + ['-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   str(repo/'tools/test_rom_loading.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
