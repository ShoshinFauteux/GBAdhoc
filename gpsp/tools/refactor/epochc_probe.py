#!/usr/bin/env python3
"""epochc_probe.py -- Epoch C step C0a-6: where does GCC put a new function that only
main() calls?  (builds/epochc-out/PLAN-EPOCHC-beta5.md §4.2.)

The question.  Epoch C moves boot, the session phases and the exits out of main() into
new static functions that only main() calls (boot_run, session_*, exit_early,
process_end).  main() is the first `.text.startup.*` input section after the ordered
text and `layout_pad_main` is the only pad in front of it, so a function emitted BEFORE
main in that run would move the frame loop with every later edit to it.

Two commands, both on a COPY of the tree (the probe is never committed and never shipped):

  epochc_probe.py patch PSP_DIR
      add seven probe functions to PSP_DIR/main_psp.c, each called from main() only
      (one is also called from a second function):
        fe_probe_plain     noinline, static, NO section attribute   <- what boot_run would be
        fe_probe_startup   noinline, section(".text.startup.fe_probe_startup")
        fe_probe_coldsec   noinline, section(".text.fe_cold.fe_probe_coldsec")
        fe_probe_coldattr  noinline, cold, no section
        fe_probe_noret     noinline, noreturn, no section           <- what process_end would be
        fe_probe_once      static, one caller, no noinline          <- boot_run WITHOUT noinline
        fe_probe_shared    noinline, no section, a second caller outside main
  epochc_probe.py report --obj OBJDUMP_H.txt --nm ELF_NM.txt [--base-nm BASE_NM.txt] [-o OUT.md]
      OBJDUMP_H.txt = `psp-objdump -h main_psp.o`, ELF_NM.txt = `psp-nm -n gpsp_adhoc.elf`
      (build.sh's .build-elf.nm).  Prints, per probe: its input section in main_psp.o,
      whether that section is emitted before or after main's in the object, its address
      in the ELF against main's, and (with --base-nm) how far main itself moved.
  epochc_probe.py --selftest
Exit: 0 ok, 1 a probe is missing where it must exist, 2 usage.
"""
import re
import sys

PROBES = ('fe_probe_plain', 'fe_probe_startup', 'fe_probe_coldsec', 'fe_probe_coldattr',
          'fe_probe_noret', 'fe_probe_once', 'fe_probe_shared')

DEFS = r'''
/* ==== EPOCH C C0a-6 SECTION PROBE (tools/refactor/epochc_probe.py): never shipped ==== */
static volatile int fe_probe_sink;
static __attribute__((noinline)) void fe_probe_plain(int x)
{ fe_probe_sink += x; }
static __attribute__((noinline, section(".text.startup.fe_probe_startup"))) void fe_probe_startup(int x)
{ fe_probe_sink += x * 3; }
static __attribute__((noinline, section(".text.fe_cold.fe_probe_coldsec"))) void fe_probe_coldsec(int x)
{ fe_probe_sink += x * 5; }
static __attribute__((noinline, cold)) void fe_probe_coldattr(int x)
{ fe_probe_sink += x * 7; }
static __attribute__((noinline, noreturn)) void fe_probe_noret(int x)
{ fe_probe_sink += x * 11; for (;;) sceKernelSleepThread(); }
static void fe_probe_once(int x)
{
   int i;
   for (i = 0; i < x; i++)
      fe_probe_sink += i * 13;
   fe_probe_sink ^= x;
}
static __attribute__((noinline)) void fe_probe_shared(int x)
{ fe_probe_sink += x * 17; }
void fe_probe_other(void);
void fe_probe_other(void)
{ fe_probe_shared(2); }
/* ==== end of the probe ==== */
'''

CALLS = r'''
   /* EPOCH C C0a-6 SECTION PROBE: the calls, from main() only */
   fe_probe_plain(argc);
   fe_probe_startup(argc);
   fe_probe_coldsec(argc);
   if (argc > 1000)
      fe_probe_coldattr(argc);
   fe_probe_once(argc);
   fe_probe_shared(argc);
   if (argc > 2000)
      fe_probe_noret(argc);
'''


def patch(psp):
    p = psp.rstrip('/\\') + '/main_psp.c'
    s = open(p, encoding='utf-8', errors='surrogateescape', newline='').read()
    if 'fe_probe_plain' in s:
        raise SystemExit('epochc_probe: %s already carries the probe' % p)
    m = re.search(r'^(/\* @extract:begin fn:main \*/\r?\n)?int main\(int argc, char \*argv\[\]\)',
                  s, re.M)
    if not m:
        raise SystemExit('epochc_probe: no `int main(int argc, char *argv[])` in %s' % p)
    a = s.find('   setup_exit_callback();', m.end())
    if a < 0:
        raise SystemExit('epochc_probe: no `setup_exit_callback();` in main()')
    a = s.index('\n', a) + 1
    s = s[:a] + CALLS.lstrip('\n') + s[a:]
    s = s[:m.start()] + DEFS.lstrip('\n') + s[m.start():]
    open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(s)
    print('epochc_probe: %d probe functions added to %s' % (len(PROBES), p))
    return 0


def parse_objdump(text):
    """[(index, name, size)] of the object's sections, in file order."""
    out = []
    for ln in text.split('\n'):
        m = re.match(r'\s*(\d+)\s+(\S+)\s+([0-9a-fA-F]{8})\s', ln)
        if m:
            out.append((int(m.group(1)), m.group(2), int(m.group(3), 16)))
    return out


def parse_nm(text):
    res = {}
    for ln in text.split('\n'):
        p = ln.split()
        if len(p) >= 3 and re.match(r'^[0-9a-fA-F]{8}$', p[0]):
            res.setdefault(p[2], int(p[0], 16))
    return res


def report(obj_txt, nm_txt, base_nm_txt=None):
    secs = parse_objdump(obj_txt)
    nm = parse_nm(nm_txt)
    by = {}
    for i, name, size in secs:
        for fn in PROBES + ('main',):
            if name == '.text.' + fn or name.endswith('.' + fn):
                by.setdefault(fn, (i, name, size))
    L, bad = [], 0
    L.append('| function | attributes | input section in main_psp.o | vs main in the object | ELF address | vs main in the ELF |')
    L.append('|---|---|---|---|---|---|')
    attrs = {'fe_probe_plain': 'noinline, no section', 'fe_probe_startup': 'noinline, section .text.startup.*',
             'fe_probe_coldsec': 'noinline, section .text.fe_cold.*', 'fe_probe_coldattr': 'noinline, cold',
             'fe_probe_noret': 'noinline, noreturn', 'fe_probe_once': 'static, one caller, no noinline',
             'fe_probe_shared': 'noinline, 2nd caller outside main', 'main': '-'}
    mi = by.get('main', (None, '?', 0))[0]
    ma = nm.get('main')
    verdict = {}
    for fn in ('main',) + PROBES:
        sec = by.get(fn)
        addr = None
        for k, v in nm.items():
            if k == fn or re.match(r'^%s\.(isra|constprop|part|cold)\b' % re.escape(fn), k):
                addr = v
                break
        if sec is None and addr is None:
            where, rel, a, er = '(none: inlined into its caller)', '-', '-', '-'
            verdict[fn] = 'inlined'
            if fn not in ('fe_probe_once',):
                bad += 1
        else:
            where = '`%s` (#%d, %d B)' % (sec[1], sec[0], sec[2]) if sec else '?'
            rel = '-' if fn == 'main' or sec is None or mi is None else (
                'BEFORE main' if sec[0] < mi else 'after main')
            a = '0x%08x' % addr if addr is not None else '?'
            er = '-' if fn == 'main' or addr is None or ma is None else (
                'BEFORE main (%+d B)' % (addr - ma) if addr < ma else 'after main (%+d B)' % (addr - ma))
            verdict[fn] = (sec[1] if sec else '?', rel, er)
        L.append('| `%s` | %s | %s | %s | %s | %s |' % (fn, attrs[fn], where, rel, a, er))
    if ma is not None:
        L.append('')
        L.append('main: 0x%08x in the probe build (mod 8192 = 0x%04x; tools/layout/layout.lock '
                 'records `at main` for the unpatched tree).' % (ma, ma & 0x1fff))
    if base_nm_txt is not None and ma is not None:
        b = parse_nm(base_nm_txt).get('main')
        if b is not None:
            L.append('')
            L.append('main: 0x%08x in the probe build, 0x%08x in the reference build: moved %+d B.'
                     % (ma, b, ma - b))
    startup = [n for _, n, _ in secs if n.startswith('.text.startup')]
    unl = [n for _, n, _ in secs if n.startswith(('.text.unlikely', '.text.hot', '.text.exit'))]
    L.append('')
    L.append('`.text.startup*` input sections of main_psp.o, in emission order: %s'
             % ', '.join('`%s`' % n for n in startup))
    L.append('`.text.unlikely*` / `.text.hot*` / `.text.exit*` input sections: %s'
             % (', '.join('`%s`' % n for n in unl) or 'none'))
    return '\n'.join(L) + '\n', bad, verdict


SELF_OBJ = '''
  5 .text.fe_cold.fe_probe_coldsec 00000018  00000000  00000000  00000100  2**2
  6 .text.startup.fe_probe_plain 00000018  00000000  00000000  00000118  2**2
  7 .text.startup.main 00007abc  00000000  00000000  00000130  2**2
  8 .text.unlikely.fe_probe_coldattr 00000018  00000000  00000000  00008000  2**2
'''
SELF_NM = '''
08804000 T main
08803fe8 t fe_probe_plain
08900000 t fe_probe_coldsec
08900100 t fe_probe_coldattr
'''


def selftest():
    txt, bad, v = report(SELF_OBJ, SELF_NM, '08804100 T main\n')
    ok = (v['fe_probe_plain'][1] == 'BEFORE main' and 'BEFORE main (-24 B)' in v['fe_probe_plain'][2]
          and v['fe_probe_coldsec'][1] == 'BEFORE main' and 'after main' in v['fe_probe_coldsec'][2]
          and v['fe_probe_coldattr'][0] == '.text.unlikely.fe_probe_coldattr'
          and v['fe_probe_once'] == 'inlined' and 'moved -256 B' in txt and bad == 3)
    print('epochc_probe selftest: %s' % ('ok' if ok else 'FAILED'))
    return 0 if ok else 1


def main(argv):
    if '--selftest' in argv:
        return selftest()
    if len(argv) >= 3 and argv[1] == 'patch':
        return patch(argv[2])
    if len(argv) >= 2 and argv[1] == 'report':
        def opt(k):
            return argv[argv.index(k) + 1] if k in argv else None
        if not opt('--obj') or not opt('--nm'):
            sys.stderr.write(__doc__)
            return 2
        rd = lambda p: open(p, encoding='utf-8', errors='replace').read()   # noqa: E731
        txt, bad, _ = report(rd(opt('--obj')), rd(opt('--nm')), rd(opt('--base-nm')) if opt('--base-nm') else None)
        sys.stdout.write(txt)
        if opt('-o'):
            open(opt('-o'), 'w', encoding='utf-8', newline='\n').write(txt)
        return 1 if bad else 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
