#!/usr/bin/env python3
"""Clone a validated fixture job, replace its build, optionally enqueue it."""
import argparse
from pathlib import Path
import re
import shutil
import perf_loop as loop


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--campaign', required=True, type=Path)
    p.add_argument('--template', required=True, type=Path)
    p.add_argument('--id', required=True)
    p.add_argument('--build', type=Path,
                   help='build_perf.py output; omit to repeat the template binary')
    p.add_argument('--enqueue', action='store_true')
    p.add_argument('--audio-oracle', action='store_true',
                   help='hash generated audio and mark this job diagnostic-only')
    p.add_argument('--smc-watch', type=lambda value: int(value, 0),
                   help='watch one IWRAM/EWRAM page; accepts a 0x-prefixed address')
    p.add_argument('--core-phase', type=int, choices=(0, 2, 3),
                   help='override the template profiler level')
    a = p.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]{1,48}',a.id): raise ValueError('invalid job id')
    original = loop.load_job(a.template)
    dest = a.campaign/'jobs'/a.id
    shutil.copytree(a.template,dest)  # Existing jobs cannot be overwritten.
    if a.build:
        build = loop.read_json(a.build/'build-result.json')
        if build['exit_code']: raise ValueError('unsuccessful build')
        for name,digest in build['artifacts'].items():
            if loop.sha(a.build/'source'/name)!=digest: raise ValueError('build artifact changed')
        for name,src in [('EBOOT.PBP','psp/EBOOT.PBP'),('gbadhoc_me.prx','psp/me/gbadhoc_me.prx')]:
            shutil.copyfile(a.build/'source'/src,dest/name)
    else:
        build = {'version':original['build'],'commit':original.get('source_commit',original['baseline_commit'])}
    ini=dest/'.gpsp-harness.ini'
    text,n=re.subn(r'^perf_job\s*=.*$', 'perf_job = '+a.id,ini.read_text(),flags=re.M)
    if n!=1: raise ValueError('ambiguous job identity in INI')
    if a.audio_oracle:
        text += '\naudio_oracle = 1\n'
    if a.smc_watch is not None:
        if not 0x02000000 <= a.smc_watch < 0x04000000:
            raise ValueError('SMC watch must be in EWRAM or IWRAM')
        text += f'\nsmc_watch = 0x{a.smc_watch:08x}\n'
    if a.core_phase is not None:
        text, n = re.subn(r'^core_phase\s*=.*$',
                          f'core_phase = {a.core_phase}', text, flags=re.M)
        if n != 1: raise ValueError('ambiguous core_phase in INI')
    ini.write_text(text)
    job=dict(original,id=a.id,build=build['version'],source_commit=build['commit'])
    if a.core_phase is not None:
        job['core_phase'] = a.core_phase
        if a.core_phase:
            job['scoring'] = False
    if a.audio_oracle:
        job['audio_oracle'] = True
        job['scoring'] = False
    if a.smc_watch is not None:
        job['smc_watch'] = f'0x{a.smc_watch:08x}'
        job['scoring'] = False
    job['files']={name:loop.sha(dest/name) for name in original['files']}
    loop.save_json(dest/'job.json',job)
    loop.load_job(dest)
    if a.enqueue:
        queue_path=a.campaign/'queue.json'
        queue=loop.read_json(queue_path)
        rel=dest.relative_to(a.campaign).as_posix()
        if rel in queue: raise ValueError('job already queued')
        loop.save_json(queue_path,queue+[rel])
    print(dest)


if __name__ == '__main__': main()
