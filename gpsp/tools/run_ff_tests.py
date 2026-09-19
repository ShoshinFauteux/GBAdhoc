#!/usr/bin/env python3
"""Run host tests against production config and ME scheduling code (native GCC).

Extract just the production functions to avoid compiling the PSP application
entrypoint and MIPS assembly on the host. No scheduling algorithm is duplicated.
"""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
source = (repo / 'psp/main_psp.c').read_text()

def function(name, next_marker):
    start = source.index('static void ' + name + '(')
    end = source.index(next_marker, start)
    return source[start:end]

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'ff_pipeline.inc').write_text(
        function('me_rend_present', '/* Called from plat_video_frame') +
        function('me_rend_frame', '/* FPS counter'))
    common = ['gcc', '-std=gnu99', '-O2', '-Wall', '-Wextra',
              '-ffunction-sections', '-fdata-sections', '-DGPSP_PLAYABLE',
              '-I' + str(repo / 'frontend-common'), '-I' + str(tmp)]
    for name, sources in (
        ('config', ['tools/test_ff_config.c', 'psp/config_psp.c',
                    'frontend-common/fe_util.c']),
        ('pipeline', ['tools/test_ff_pipeline.c']),
    ):
        binary = tmp / name
        subprocess.run(common + [str(repo / p) for p in sources] +
                       ['-Wl,--gc-sections', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
