#!/usr/bin/env python3
"""Build an isolated, source-pinned PSP performance package using Docker."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

IMAGE = 'pspdev/pspdev@sha256:b22811072ea0721d7f6c666a5a279b6def2b0cd95892b819d9570f8ffc4452c0'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--core-option', action='append', default=[],
                        help='one make option such as SMC_GATES_CLUSTER=1')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    branch = subprocess.check_output(['git','branch','--show-current'],cwd=repo,text=True).strip()
    if not branch or branch in ('main','master'): raise RuntimeError('feature branch required')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+',args.version): raise ValueError('invalid version')
    if any(not re.fullmatch(r'[A-Z][A-Z0-9_]*=[0-9]+', value)
           for value in args.core_option):
        raise ValueError('core options must be NAME=unsigned-integer')
    audit = args.output.resolve()
    audit.mkdir(parents=True,exist_ok=False)
    source = audit/'source'
    names = subprocess.check_output(['git','ls-files','-z'],cwd=repo).decode().split('\0')
    names += ['psp/perf_rig.h']
    manifest = {}
    for name in sorted(set(names)-{''}):
        src = repo/name
        if not src.is_file() or name.startswith('releases/'): continue
        dst = source/name
        dst.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(src,dst)
        manifest[name] = hashlib.sha256(src.read_bytes()).hexdigest()
    (audit/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (audit/'patch.diff').write_bytes(subprocess.check_output(['git','diff','HEAD','--binary'],cwd=repo))
    options = ' '.join(args.core_option)
    (audit/'build.sh').write_text(f'''#!/bin/sh
set -eu
cd /build
make -C psp clean
make platform=psp1 clean
make platform=psp1 GIT_VERSION={args.version} SMC_GATES=1 SMC_GATES_SIMPLE=1 SMC_GATES_RANKED=1 GBA_PC_MASK=1 BADJUMP_SAFE=1 {options} -j4
make -C psp EXTRA_DEFS='-DGPSP_PLAYABLE -DVID_TRIPLE -DGPSP_KEEP_TELEMETRY -DGPSP_PERF_RIG' PSP_EBOOT_TITLE='GBAdhoc Perf'
sha256sum psp/EBOOT.PBP psp/me/gbadhoc_me.prx
''',newline='\n')
    command=['docker','run','--rm','-v',source.as_posix()+':/build','-v',audit.as_posix()+':/audit',IMAGE,'sh','/audit/build.sh']
    with (audit/'build.log').open('wb') as log:
        result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
    metadata={'command':command,'exit_code':result.returncode,'branch':branch,
              'commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
              'version':args.version,'core_options':args.core_option}
    if not result.returncode:
        metadata['artifacts']={name:hashlib.sha256((source/name).read_bytes()).hexdigest()
                              for name in ('psp/EBOOT.PBP','psp/me/gbadhoc_me.prx')}
    (audit/'build-result.json').write_text(json.dumps(metadata,indent=2)+'\n')
    if result.returncode or 'stubs out of order' in (audit/'build.log').read_text(errors='replace'):
        raise RuntimeError('PSP build failed; see build.log')
    print(json.dumps(metadata,indent=2))


if __name__ == '__main__': main()
