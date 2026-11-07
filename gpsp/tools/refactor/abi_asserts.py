#!/usr/bin/env python3
"""abi_asserts.py -- T14 (PLAN §5; r1 H-ABI, r2 H-8): generate fe_abi_asserts.h, the
compile-time proof that psp/main_psp.c's HAND-COPIED core declarations still match the
core's own headers.

main_psp.c redeclares core functions and globals instead of including the core headers
(main_psp.c:1108-1112: frontend-common stays core-agnostic; 4851-4853: including
gba_memory.h conflicts).  Nothing checks the copies: core_phase_take has 21 parameters
(r1 H-ABI), VRAM_DIRTY_PAGES and the MER_* constants are copied by hand (r2 H-8, and
tools/test_ff_pipeline.c:12-15 copies MER_* again).

How:
 1. The frontend side is read from `psp-gcc -E` of main_psp.c for the flavour (so #if'ed
    copies are per flavour): every `extern` declaration (file or block scope) and every
    file-scope prototype of a function main_psp.c does not define itself.
 2. Each becomes `typedef <declaration with NAME -> abi_fe_t_NAME>;` and
    `_Static_assert(__builtin_types_compatible_p(__typeof__(&NAME), abi_fe_t_NAME *), ...)`.
    Comparing POINTERS keeps qualifiers (const/volatile) in the comparison; a bare
    __builtin_types_compatible_p ignores top-level qualifiers.
 3. The check TU is `#include "common.h"` (the core's header chain: main.h, gba_memory.h,
    cpu.h, ...) compiled with the flavour's exact CORE compile line (-fsyntax-only, docker).
    A discovery pass drops names the core headers do not declare and frontend types the
    core TU cannot see; they are listed in the header as UNCHECKED, with the reason.
 4. Constants: VRAM_DIRTY_SHIFT/PAGES against gba_memory.h; MER_INPUT_US, MER_RETIRE_US,
    MER_MISS_LIMIT and VRAM_DIRTY_PAGES against tools/test_ff_pipeline.c's copies.
Nothing is wired into the emulator build: the header is generated into --out and only
compiled by this tool's own check TU.

Usage:
  abi_asserts.py gen --flavour-dir RUN/<fl> --src GPSP --out DIR [--main-c F] [--core-h H]
        -> DIR/fe_abi_asserts.h, DIR/abi_check.c, DIR/report.txt; exit 1 if it does not compile
  abi_asserts.py all --run RUN --src GPSP --out DIR      (release, lab, harness)
Exit: 0 generated and compiles, 1 an assertion or compile failure, 2 usage.
"""
import argparse
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import aec_cparse as C  # noqa: E402
import aec_pspcc as P  # noqa: E402

FLAVOURS = ('release', 'lab', 'harness')
CORE_MACROS = ['VRAM_DIRTY_SHIFT', 'VRAM_DIRTY_PAGES']
TEST_MACROS = ['MER_INPUT_US', 'MER_RETIRE_US', 'MER_MISS_LIMIT', 'VRAM_DIRTY_PAGES']
TEST_FILE = 'tools/test_ff_pipeline.c'


# ------------------------------------------------------------- extraction
def strip_attrs(toks):
    out, i = [], 0
    while i < len(toks):
        if toks[i].text in ('__attribute__', '__asm__', 'asm') and i + 1 < len(toks) and toks[i + 1].text == '(':
            d, j = 0, i + 1
            while j < len(toks):
                if toks[j].text == '(':
                    d += 1
                elif toks[j].text == ')':
                    d -= 1
                    if d == 0:
                        break
                j += 1
            i = j + 1
            continue
        out.append(toks[i])
        i += 1
    return out


def declarator_name(toks):
    """name of a single declarator: '(*name)(...)' or the identifier before the first ( [ ."""
    for i in range(len(toks) - 2):
        if toks[i].text == '(' and toks[i + 1].text == '*' and toks[i + 2].kind == 'id':
            return i + 2
    for i, t in enumerate(toks):
        if t.text in ('(', '[', '=', ';'):
            for j in range(i - 1, -1, -1):
                if toks[j].kind == 'id' and toks[j].text not in C.KEYWORDS:
                    return j
            return None
    for j in range(len(toks) - 1, -1, -1):
        if toks[j].kind == 'id' and toks[j].text not in C.KEYWORDS:
            return j
    return None


def split_declarators(toks):
    """'extern u32 a, b[4];' -> [[extern u32 a], [extern u32 b [ 4 ]]] (not for functions)."""
    # specifier tokens = up to the first declarator start
    parts, cur, d = [], [], 0
    for t in toks:
        if t.text in ('(', '['):
            d += 1
        elif t.text in (')', ']'):
            d -= 1
        if t.text == ',' and d == 0:
            parts.append(cur)
            cur = []
            continue
        cur.append(t)
    parts.append(cur)
    if len(parts) == 1:
        return parts
    first = parts[0]
    k = declarator_name(first)
    spec = first[:k]
    while spec and spec[-1].text == '*':
        spec = spec[:-1]
    return [first] + [spec + p for p in parts[1:]]


def extract(toks):
    """-> [(name, line, decl_tokens_without_semicolon, kind)] of foreign declarations."""
    defined = set(C.functions(toks))
    own_vars = set()
    res = []
    # statements at file scope and extern statements anywhere
    i, n, depth = 0, len(toks), 0
    stmt = []
    while i < n:
        t = toks[i]
        if t.text == '{':
            if depth == 0 and stmt and stmt[-1].text == ')':
                # function body: skip it, but scan inside for block-scope externs
                j = C.match_close(toks, i)
                body = toks[i + 1:j]
                res += _block_externs(body)
                stmt = []
                i = j + 1
                continue
            depth += 1
        elif t.text == '}':
            depth -= 1
        if t.text == ';' and depth == 0:
            _file_stmt(stmt, defined, own_vars, res)
            stmt = []
        else:
            stmt.append(t)
        i += 1
    # externs of variables main_psp.c defines itself are not copies
    out = []
    seen = set()
    for r in res:
        if r[0] in defined or r[0] in own_vars:
            continue
        if (r[0], tuple(x.text for x in r[2])) in seen:
            continue
        seen.add((r[0], tuple(x.text for x in r[2])))
        out.append(r)
    return out


def _block_externs(body):
    res, stmt = [], []
    for t in body:
        if t.text == ';':
            if stmt and stmt[0].text == 'extern':
                for d in split_declarators(strip_attrs(stmt)):
                    k = declarator_name(d)
                    if k is not None:
                        res.append((d[k].text, d[k].line, d, 'block'))
            stmt = []
        elif t.text in ('{', '}'):
            stmt = []
        else:
            stmt.append(t)
    return res


def _file_stmt(stmt, defined, own_vars, res):
    if not stmt:
        return
    w = [t.text for t in stmt]
    if w[0] in ('typedef', 'struct', 'union', 'enum') and '(' not in w[:3]:
        if w[0] != 'typedef':
            pass
        return
    if 'typedef' in w or w[0] == '_Static_assert':
        return
    toks = strip_attrs(stmt)
    w = [t.text for t in toks]
    if not w:
        return
    if w[0] == 'extern':
        for d in split_declarators(toks):
            k = declarator_name(d)
            if k is not None:
                res.append((d[k].text, d[k].line, d, 'extern'))
        return
    if 'static' in w or 'inline' in w:
        # a static definition/prototype is the TU's own
        k = declarator_name(toks)
        if k is not None:
            own_vars.add(toks[k].text)
        return
    if '(' in w and '=' not in w:
        k = declarator_name(toks)
        if k is not None and k + 1 < len(toks) and toks[k + 1].text == '(':
            res.append((toks[k].text, toks[k].line, toks, 'proto'))
            return
    k = declarator_name(toks)
    if k is not None:
        own_vars.add(toks[k].text)       # a definition of a global (with or w/o initializer)


KNOWN = os.path.join(HERE, 'abi_asserts.known')
# The names asserted at the reviewed baseline, per flavour.  The discovery pass moves a
# copy to UNCHECKED whenever its typedef does not compile in the core TU, so without
# this pin a copy retyped to a frontend-only typedef (`extern fe_count_t x;` with
# fe_count_t = u16, where the core says u32) silently stopped being checked and the
# tool still passed (verifier 2026-11-05).  A pinned name that is no longer asserted
# FAILS; `--write-pin` rewrites a flavour's entries after review.
PIN = os.path.join(HERE, 'abi_asserts.pinned')


def load_pin(path, flavour):
    if not path or not os.path.exists(path):
        return None
    out = set()
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        p = line.rstrip('\n').split('\t')
        if p[0] == flavour:
            out.add(p[1])
    return out


def write_pin(path, flavour, names):
    keep = []
    if os.path.exists(path):
        keep = [l for l in open(path, encoding='utf-8')
                if not l.startswith('#') and l.strip() and l.split('\t')[0] != flavour]
    with open(path, 'w', newline='\n', encoding='utf-8') as f:
        f.write('# T14 pinned asserted copies: flavour <TAB> name.  Written by abi_asserts.py\n'
                '# --write-pin after review; a pinned name that is no longer asserted fails gen.\n')
        f.writelines(sorted(keep))
        for n in sorted(names):
            f.write('%s\t%s\n' % (flavour, n))


def load_known(path=KNOWN):
    k = {}
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            p = line.rstrip('\n').split('\t')
            k[p[0]] = (p[1], p[2] if len(p) > 2 else '')
    return k


def typedef_of(name, d, norm=None):
    toks = [t for t in d if t.text not in ('extern',)]
    k = next(i for i, t in enumerate(toks) if t.text == name)
    txt = []
    for i, t in enumerate(toks):
        txt.append('abi_fe_t_' + name if i == k else t.text)
    if norm == 'uint':
        s, out, i = txt, [], 0
        while i < len(s):
            if s[i] == 'unsigned' and i + 1 < len(s) and s[i + 1] == 'int':
                out.append('u32')
                i += 2
                continue
            if s[i] == 'unsigned' and (i + 1 >= len(s) or s[i + 1] not in ('char', 'short', 'long')):
                out.append('u32')
                i += 1
                continue
            out.append(s[i])
            i += 1
        txt = out
    elif norm == 'ret=bool':
        j = next(i for i, x in enumerate(txt) if x == 'int')
        txt[j] = 'bool'
    return 'typedef ' + ' '.join(txt) + ';'


def macro_values(path, names):
    vals = {}
    for i, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
        m = re.match(r'\s*#\s*define\s+(\w+)\s+(.*?)\s*(/\*.*)?$', line)
        if m and m.group(1) in names and m.group(1) not in vals:
            vals[m.group(1)] = (re.sub(r'//.*', '', m.group(2)).strip(), i)
    return vals


def prefixed(expr, names, pre):
    return re.sub(r'\b(%s)\b' % '|'.join(map(re.escape, names)), lambda m: pre + m.group(1), expr)


# ------------------------------------------------------------- generation
def render(flavour, items, unchecked, macros_fe, macros_test, src_desc, known=None):
    known = known or {}
    L = ['/* fe_abi_asserts.h -- GENERATED by gpsp/tools/refactor/abi_asserts.py; do not edit.',
         ' * Flavour: %s.  Source: %s.' % (flavour, src_desc),
         ' * Include AFTER the core headers (common.h) in a TU compiled with the core flags.',
         ' * Each assert compares main_psp.c\'s hand-copied declaration with the core\'s own.',
         ' * checked: %d declarations, %d constants; unchecked: %d (listed at the end). */' % (
             len(items), len(macros_fe) + len(macros_test), len(unchecked)),
         '#ifndef FE_ABI_ASSERTS_H', '#define FE_ABI_ASSERTS_H', '']
    for name, line, d, kind in items:
        kn = known.get(name)
        L.append('/* main_psp.c:%d (%s)%s */' % (line, kind, (' KNOWN, normalised %s: %s' % kn) if kn else ''))
        L.append(typedef_of(name, d, kn[0] if kn else None))
        L.append('_Static_assert(__builtin_types_compatible_p(__typeof__(&%s), abi_fe_t_%s *),\n'
                 '               "main_psp.c:%d %s: the frontend copy differs from the core declaration");'
                 % (name, name, line, name))
    if macros_fe:
        L.append('')
        L.append('/* constants copied from the core (r2 H-8) */')
        names = list(macros_fe)
        for nm, (val, line) in macros_fe.items():
            L.append('#define ABI_FE_%s (%s)   /* main_psp.c:%d */' % (nm, prefixed(val, names, 'ABI_FE_'), line))
        for nm, (val, line) in macros_fe.items():
            L.append('_Static_assert(ABI_FE_%s == (%s), "main_psp.c:%d %s differs from the core header");'
                     % (nm, nm, line, nm))
    if macros_test:
        L.append('')
        L.append('/* constants copied again by %s (r2 H-8) */' % TEST_FILE)
        for nm, (val, line, feval, feline) in macros_test.items():
            L.append('#define ABI_TEST_%s (%s)   /* %s:%d */' % (nm, val, TEST_FILE, line))
            L.append('#define ABI_FET_%s (%s)   /* main_psp.c:%d */' % (
                nm, prefixed(feval, list(macros_fe) + [nm], 'ABI_FE_') if nm in macros_fe else
                prefixed(feval, [], ''), feline))
            L.append('_Static_assert(ABI_FET_%s == ABI_TEST_%s, "%s:%d %s differs from main_psp.c:%d");'
                     % (nm, nm, TEST_FILE, line, nm, feline))
    L.append('')
    L.append('/* the sleep-quiesce stop protocol (r1 H-SQ, PLAN §1.4): audio_stop / io_thread_stop')
    L.append(' * return 0 = joined, 1 = none, which must stay SQ_DONE / SQ_NONE (psp/sleep_q.h:37-38) */')
    L.append('#ifndef SQ_DONE')
    L.append('#error "fe_abi_asserts.h: include psp/sleep_q.h first"')
    L.append('#endif')
    L.append('_Static_assert(SQ_DONE == 0 && SQ_NONE == 1, "sleep_q.h: SQ_DONE/SQ_NONE are not 0/1");')
    L.append('')
    L.append('/* UNCHECKED (%d): no core declaration to compare with, or a frontend-only type.' % len(unchecked))
    for name, line, why in unchecked:
        L.append(' *   main_psp.c:%-6d %-36s %s' % (line, name, why))
    L.append(' */')
    L.append('#endif')
    return '\n'.join(L) + '\n'


def compile_check(src, flavour_dir, workdir, header_name='fe_abi_asserts.h', core_h=None):
    """psp-gcc -fsyntax-only of workdir/abi_check.c with the core's main.o compile line."""
    argv = P.compile_argv(flavour_dir, 'main.o', '.build-core.log')
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
    out.pop()
    out += ['-fsyntax-only', '-fmax-errors=0', '-I/work', '/work/abi_check.c']
    mounts = [(workdir, '/work', 'ro')]
    if core_h:
        mounts.append((core_h[0], '/build/' + core_h[1], 'ro'))
    return P.docker_run(src, '/build', ' '.join(shlex.quote(a) for a in out), mounts)


def gen(flavour_dir, src, outdir, flavour, main_c=None, core_h=None, test_c=None, known_path=KNOWN,
        pin_path=PIN):
    os.makedirs(outdir, exist_ok=True)
    known = load_known(known_path)
    ip = os.path.join(outdir, 'main_psp.i')
    with open(ip, 'w', newline='\n') as f:
        f.write(P.preprocess(src, flavour_dir, override=main_c))
    toks = C.load_pp(ip)
    items = extract(toks)
    unchecked = []
    main_raw = main_c or os.path.join(src, 'psp', 'main_psp.c')
    mfe = macro_values(main_raw, set(CORE_MACROS) | set(TEST_MACROS))
    macros_fe = {k: mfe[k] for k in CORE_MACROS if k in mfe}
    tvals = macro_values(test_c or os.path.join(src, TEST_FILE), set(TEST_MACROS))
    macros_test = {k: (tvals[k][0], tvals[k][1], mfe[k][0], mfe[k][1]) for k in TEST_MACROS
                   if k in tvals and k in mfe}
    with open(os.path.join(outdir, 'abi_check.c'), 'w', newline='\n') as f:
        f.write('/* T14 check TU: the core header chain, then the generated asserts. */\n'
                '#include "common.h"\n#include "psp/sleep_q.h"\n#include "fe_abi_asserts.h"\n')
    hp = os.path.join(outdir, 'fe_abi_asserts.h')
    desc = 'main_psp.c%s via psp-gcc -E' % (' (MUTANT %s)' % os.path.basename(main_c) if main_c else '')
    # discovery: drop items whose name the core does not declare or whose type does not compile
    for rnd in range(6):
        with open(hp, 'w', newline='\n') as f:
            f.write(render(flavour, items, unchecked, macros_fe, macros_test, desc, known))
        rc, so, se = compile_check(src, flavour_dir, outdir, core_h=core_h)
        if rc == 0:
            break
        hl = open(hp).read().split('\n')
        drop = {}
        for m in re.finditer(r'fe_abi_asserts\.h:(\d+):\d+: error: (.*)', se):
            ln, msg = int(m.group(1)), m.group(2)
            if 'static assertion failed' in msg:
                continue                        # a real mismatch: never dropped
            # which item does the line belong to?  the typedef line names abi_fe_t_NAME
            txt = hl[ln - 1] if ln - 1 < len(hl) else ''
            mm = re.search(r'abi_fe_t_(\w+)', txt) or re.search(r'__typeof__\(&(\w+)\)', txt)
            if not mm and ln >= 2:
                mm = re.search(r'__typeof__\(&(\w+)\)', hl[ln - 2])
            if mm:
                nm = mm.group(1)
                why = ('no core declaration' if ('undeclared' in msg and "'%s'" % nm in msg)
                       else 'frontend type not visible in the core TU: ' + msg[:70])
                drop.setdefault(nm, why)
        if not drop:
            break
        keep = []
        for it in items:
            if it[0] in drop:
                unchecked.append((it[0], it[1], drop[it[0]]))
            else:
                keep.append(it)
        items = keep
    with open(hp, 'w', newline='\n') as f:
        f.write(render(flavour, items, unchecked, macros_fe, macros_test, desc, known))
    rc, so, se = compile_check(src, flavour_dir, outdir, core_h=core_h)
    fails = [l for l in se.split('\n') if 'error' in l]
    rep = ['T14 %s: %d copied declarations found, %d asserted, %d unchecked; %d constants asserted'
           % (flavour, len(items) + len(unchecked), len(items), len(unchecked),
              len(macros_fe) + len(macros_test)),
           'compile (psp-gcc -fsyntax-only, core flags): %s' % ('ok' if rc == 0 else 'FAILED'),
           'known mismatches normalised (abi_asserts.known): %s' % (
               ', '.join(n for n in known if n in set(i[0] for i in items)) or 'none')]
    unused = [n for n in known if n not in set(i[0] for i in items)]
    if unused:
        rep.append('known entries with no matching copy in this flavour (stale?): %s' % ', '.join(unused))
    asserted = set(i[0] for i in items)
    pin_fail = 0
    if pin_path == 'WRITE':
        write_pin(PIN, flavour, asserted)
        rep.append('pin: wrote %d asserted names for %s to %s' % (len(asserted), flavour, PIN))
    else:
        pinned = load_pin(pin_path, flavour)
        if pinned is None:
            rep.append('pin: none (%s missing): asserted set not held to a baseline' % pin_path)
        else:
            why = {n: w for n, _, w in unchecked}
            for n in sorted(pinned - asserted):
                pin_fail += 1
                rep.append('  PIN FAIL %s: asserted at the pinned baseline, now %s' % (
                    n, ('UNCHECKED (%s)' % why[n]) if n in why else 'no copy found'))
            new = sorted(asserted - pinned)
            rep.append('pin: %d pinned, %d lost, %d new%s' % (
                len(pinned), pin_fail, len(new), (' (review, then --write-pin): ' + ', '.join(new)) if new else ''))
    rep += ['  ' + l for l in fails[:30]]
    with open(os.path.join(outdir, 'report.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(rep) + '\n' + se)
    print('\n'.join(rep))
    return 0 if rc == 0 and not pin_fail else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    g = sub.add_parser('gen')
    g.add_argument('--flavour-dir', required=True)
    g.add_argument('--flavour')
    g.add_argument('--src', required=True)
    g.add_argument('--out', required=True)
    g.add_argument('--main-c', help='a mutated copy of psp/main_psp.c (bind-mounted over the tree)')
    g.add_argument('--core-h', nargs=2, metavar=('FILE', 'TREEPATH'),
                   help='a mutated copy of a core header, e.g. /tmp/main.h main.h')
    g.add_argument('--test-c', help='a mutated copy of tools/test_ff_pipeline.c')
    g.add_argument('--known', default=KNOWN)
    g.add_argument('--pin', default=PIN, help='pinned asserted names (abi_asserts.pinned)')
    g.add_argument('--write-pin', action='store_true', help='rewrite this flavour\'s pin (after review)')
    a = sub.add_parser('all')
    a.add_argument('--run', required=True)
    a.add_argument('--src', required=True)
    a.add_argument('--out', required=True)
    a.add_argument('--write-pin', action='store_true')
    o = ap.parse_args()
    if o.cmd == 'gen':
        fl = o.flavour or os.path.basename(os.path.normpath(o.flavour_dir))
        return gen(o.flavour_dir, o.src, o.out, fl, o.main_c, o.core_h, o.test_c, o.known,
                   'WRITE' if o.write_pin else o.pin)
    if o.cmd == 'all':
        rc = 0
        for fl in FLAVOURS:
            rc |= gen(os.path.join(o.run, fl), o.src, os.path.join(o.out, fl), fl,
                      pin_path='WRITE' if o.write_pin else PIN)
        return rc
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
