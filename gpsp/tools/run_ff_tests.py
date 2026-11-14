#!/usr/bin/env python3
"""Run host tests against production config and ME scheduling code (native GCC).

Extract just the production functions to avoid compiling the PSP application
entrypoint and MIPS assembly on the host. No scheduling algorithm is duplicated.

The multiplier cadence (1.5x / 2x / 3x, tools/test_ff_cadence.c) runs the real
ceiling, draw rule and flip hold, then MUTANTS: each seeded change to the
production text (the fragment, psp/ff_gate.h, the paced loop, config_psp.c)
must make a test fail.  `--no-mutants` skips them.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(repo / 'tools' / 'tests'))
import extract  # noqa: E402

source = extract.read_source(str(repo / 'psp/main_psp.c'))
plain = extract.strip_markers(source)

def function(name, next_marker, ret='void'):
    # read through its @extract marker ff:NAME: from the definition up to next_marker
    text = extract.extract(source, 'ff:' + name)
    assert text.startswith('static ' + ret + ' ' + name + '('), name
    assert text + next_marker in plain, name + ': the region does not end at ' + next_marker
    return text

def cadence_text():
    text = extract.extract(source, 'ff:ffh_cadence')
    assert text.startswith('static __attribute__((noinline)) int ffh_too_soon('), 'ff:ffh_cadence start'
    for fn in ('static int ffh_hold(void)', 'static unsigned ffh_cap_mult(void)',
               'static __attribute__((noinline)) void ffh_cap(void)',
               'static __attribute__((noinline)) int ffh_cap_draw(void)'):
        assert fn in text, 'ff:ffh_cadence lacks ' + fn
    return text

def mult_frames_text():
    text = extract.extract(source, 'ff:mult_frames')
    assert 'g_ffm_acc' in text and 'n = ' in text, 'ff:mult_frames'
    return text

GATE_H = (repo / 'psp/ff_gate.h').read_text()
CONFIG_C = (repo / 'psp/config_psp.c').read_text()

# (name, which text, old, new): each must make the cadence or config test fail.
MUTANTS = [
    ('draw rule off: the gate draws 1.5x/2x', 'cadence', 'm < 30u && g_ffc_on', 'm < 10u && g_ffc_on'),
    ('ceiling never records its block', 'cadence', '   if (blocked)\n      g_ffc_bvc = sceDisplayGetVcount();', ''),
    ('leaving the rule does not re-arm the gate', 'cadence',
     '      ff_gate_arm(&g_ff_gate_st, g_ff_gate_st.period_us);\n', ''),
    ('quota one frame too many', 'gate', '- (int)mult_x10;\n}', '- (int)mult_x10 - 10;\n}'),
    ('last frame off by one', 'gate', 'ff_cap_over(mult_x10, df + 1u, dv)', 'ff_cap_over(mult_x10, df, dv)'),
    ('on-pace test inverted', 'gate', 'mult_x10 * dv) >= 0;', 'mult_x10 * dv) < 0;'),
    ('paced loop drops the owed tenths', 'mult', 'g_ffm_acc %= 10u;', 'g_ffm_acc = 0;'),
    ('1.5x saved as 1.5x in the legacy key', 'config', 'g_pcfg.ff_mult_x10 ? 30 : 0', 'g_pcfg.ff_mult_x10'),
    ('ff_speed_x10 ignored on load', 'config', 'sp == 15 || sp == 20 ? sp : 30', '30'),
    ('legacy 15 read as 1.5x without ff_speed_x10', 'config',
     'int sp = (int)fe_ini_get_int(cfg_path, "ff_speed_x10", 30);',
     'int sp = (int)fe_ini_get_int(cfg_path, "ff_speed_x10", g_pcfg.ff_mult_x10);'),
    ('menu list out of order', 'config', '"1.5x", "2x", "3x"', '"2x", "1.5x", "3x"'),
    ('2x maps to 3x', 'config', '{ 15, 20, 30, 0, 0 }', '{ 15, 30, 30, 0, 0 }'),
]

def build_cadence(tmp, common, cadence, gate, mult, config, quiet=False):
    (tmp / 'ff_cadence.inc').write_text(cadence)
    (tmp / 'ff_mult_frames.inc').write_text(mult)
    (tmp / 'ff_gate.h').write_text(gate)
    (tmp / 'config_psp.c').write_text(config)
    binaries = []
    for name, sources in (
        ('cadence', ['tools/test_ff_cadence.c']),
        ('config', ['tools/test_ff_config.c']),
    ):
        binary = tmp / name
        cmd = (common + ['-I' + str(repo / 'psp')] +
               [str(repo / p) for p in sources] +
               [str(tmp / 'config_psp.c'), str(repo / 'psp/ctl_map.c'),
                str(repo / 'frontend-common/fe_util.c'),
                '-Wl,--gc-sections', '-o', str(binary)])
        rc = subprocess.run(cmd, capture_output=quiet)
        if rc.returncode:
            return False
        binaries.append(binary)
    for b in binaries:
        rc = subprocess.run([str(b)], capture_output=quiet)
        if rc.returncode:
            return False
    return True

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'ff_pipeline.inc').write_text(
        function('mer_retire_early_active', 'static int mer_lowlat_active', 'int') +
        function('me_rend_present', '/* Called from plat_video_frame') +
        function('me_rend_frame', '/* FPS counter'))
    common = ['gcc', '-std=gnu99', '-O2', '-Wall', '-Wextra',
              '-ffunction-sections', '-fdata-sections', '-DGPSP_PLAYABLE',
              '-I' + str(repo / 'frontend-common'), '-I' + str(tmp)]
    # display_profiles rides here because it is the same shape -- the real
    # config_psp.c and INI writer -- and run_host_tests.py caps the number
    # of suites (docs/DISPLAY-FEATURES.md).
    for name, sources, extra in (
        ('config', ['tools/test_ff_config.c', 'psp/config_psp.c',
                    'psp/ctl_map.c', 'frontend-common/fe_util.c'], []),
        ('pipeline', ['tools/test_ff_pipeline.c'], []),
        ('display_profiles', ['tools/test_display_profiles.c',
                              'psp/config_psp.c', 'psp/ctl_map.c',
                              'frontend-common/fe_util.c'],
         ['-Wl,--wrap=fopen']),
    ):
        binary = tmp / name
        subprocess.run(common + [str(repo / p) for p in sources] + extra +
                       ['-Wl,--gc-sections', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    texts = {'cadence': cadence_text(), 'gate': GATE_H,
             'mult': mult_frames_text(), 'config': CONFIG_C}
    ctmp = tmp / 'cadence'
    ctmp.mkdir()
    ccommon = [c if not c.startswith('-I' + str(tmp)) else '-I' + str(ctmp) for c in common]
    ccommon = ccommon[:1] + ['-I' + str(ctmp)] + ccommon[1:]
    if not build_cadence(ctmp, ccommon, texts['cadence'], texts['gate'],
                         texts['mult'], texts['config']):
        sys.exit('FAIL: ff cadence')
    if '--no-mutants' not in sys.argv:
        caught = 0
        for what, which, old, new in MUTANTS:
            assert texts[which].count(old) == 1, 'mutant anchor not unique: ' + what
            t = dict(texts)
            t[which] = t[which].replace(old, new)
            mdir = tmp / ('m%d' % caught)
            mdir.mkdir()
            mcommon = [c if c != '-I' + str(ctmp) else '-I' + str(mdir) for c in ccommon]
            if build_cadence(mdir, mcommon, t['cadence'], t['gate'], t['mult'],
                             t['config'], quiet=True):
                sys.exit('FAIL: mutant survived: ' + what)
            print('  mutant caught: ' + what)
            caught += 1
            shutil.rmtree(mdir)
        print('PASS: ff cadence mutants %d/%d caught' % (caught, len(MUTANTS)))
