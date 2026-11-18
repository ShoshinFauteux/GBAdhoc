"""Shared helpers for the elf-func group's tests (func_diff T1, loop_diff
T1b, inline_census T13): baseline locations, ELF byte mutation, a tiny
check() counter.  Not a tool; imported by tests/elf-func_test_*.py."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

TESTS = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(TESTS)
sys.path.insert(0, TOOLS)
from elf32 import Elf  # noqa: E402

# checkout/gpsp/tools/refactor -> the folder that holds the checkout (local outputs, not in the repo)
BUILDS = os.path.abspath(os.path.join(TOOLS, '..', '..', '..', '..'))
OUT = os.environ.get('EF_OUT', os.path.join(BUILDS, 'mainpsp-tools-out'))
BASELINE = os.environ.get('EF_BASELINE', os.path.join(OUT, 'baseline'))
PAD144 = os.environ.get('EF_PAD144', os.path.join(OUT, 'scratch-elf-func', 'pad144', 'release',
                                                  'gpsp_adhoc.elf'))
SRC = os.environ.get('EF_SRC', os.path.join(TOOLS, '..', '..', 'psp', 'main_psp.c'))
HOT_ORDER = os.path.join(TOOLS, '..', '..', 'psp', 'layout', 'hot_order.txt')
FLAVOURS = ('release', 'lab', 'harness')


def elf(run, flavour):
    return os.path.join(BASELINE, run, flavour, 'gpsp_adhoc.elf')


class Checks:
    def __init__(self, name):
        self.name, self.ok, self.fail, self.skip = name, 0, 0, 0

    def check(self, cond, msg):
        if cond:
            self.ok += 1; print('  ok   ' + msg)
        else:
            self.fail += 1; print('  FAIL ' + msg)
        return cond

    def skipped(self, msg):
        self.skip += 1; print('  skip ' + msg)

    def done(self):
        print('%s: %d ok, %d failed, %d skipped' % (self.name, self.ok, self.fail, self.skip))
        return 1 if self.fail else 0


def run_tool(tool, *args):
    """run a tool, return (rc, stdout)."""
    p = subprocess.run([sys.executable, os.path.join(TOOLS, tool)] + [str(a) for a in args],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


class Mutant:
    """Copy an ELF next to its manifest-free temp dir and patch words."""

    def __init__(self, src):
        self.src = src
        self.e = Elf(src)
        self.d = bytearray(self.e.data)
        self.dir = tempfile.mkdtemp(prefix='elf-func-mut-')
        self.notes = []

    def func(self, name):
        hits = [s for s in self.e.funcs() if s['name'] == name]
        if not hits:
            raise LookupError(name)
        return hits[0]

    def off(self, addr):
        sec = self.e.section_of(addr)
        return sec['offset'] + addr - sec['addr']

    def word(self, addr):
        return struct.unpack_from('<I', self.d, self.off(addr))[0]

    def put(self, addr, w, note):
        struct.pack_into('<I', self.d, self.off(addr), w & 0xffffffff)
        self.notes.append('%08x: %s' % (addr, note))

    def save(self, name='mut.elf'):
        p = os.path.join(self.dir, name)
        open(p, 'wb').write(self.d)
        return p

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)
