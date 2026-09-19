#!/usr/bin/env python3
"""Run the production cart and parcel parser against Android-emitted fixtures."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / 'test_mgift'
    subprocess.run([
        'gcc', '-std=c99', '-g', '-fsanitize=address,undefined', '-Wall', '-Wextra',
        '-Ifrontend-common', '-Ilibretro/libretro-common/include',
        'tools/test_mgift.c', 'psp/mgift_payload.c', '-o', str(binary),
    ], cwd=repo, check=True)
    for fixture in sorted((repo / 'tools/fixtures/mgift').glob('*.mgc2')):
        print(fixture.name, flush=True)
        subprocess.run([str(binary), str(fixture)], check=True)
