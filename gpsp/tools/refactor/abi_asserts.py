#!/usr/bin/env python3
"""abi_asserts.py -- T14 (PLAN §5; r1 H-ABI, r2 H-8): generate fe_abi_asserts.h, the
compile-time proof that psp/main_psp.c's HAND-COPIED core declarations still match the
core's own headers.

main_psp.c redeclares core functions and globals instead of including the core headers
(at 01067e74, main_psp.c:1108-1112: frontend-common stays core-agnostic; 4851-4853: including
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
    cpu.h, ...) and `#include "psp/sleep_q.h"`, compiled with the flavour's exact CORE compile
    line (-fsyntax-only, docker).  A discovery pass drops the names those headers do not
    declare (whether or not some other header does: gbcore_prepare is in gbcore/gbcore.h,
    which the check TU does not include) and frontend types the core TU cannot see; they are
    listed in the header as UNCHECKED, with the reason.  So a flavour's header lists every
    copy of THAT flavour's -E, asserted or UNCHECKED (P-review r2, N4).  A copy in an #if arm
    the flavour does not compile is in neither list; `unbuilt` (below) lists the copy sites
    that no generated flavour has (P-review r3, B6).
 3b. PLATFORM (tier C, C6-1; health-out/design/C-6.md): a copy whose name abi_asserts.platform
    lists (linker symbols, the PSP_MODULE_INFO expansion, an SDK function its header lacks) is not
    core ABI.  It is listed in the header's PLATFORM block with that file's reason, not as
    UNCHECKED, so UNCHECKED holds only copies of core (or other tree) names.  The block's rows
    have the UNCHECKED rows' form, so `unbuilt` counts them as cited.  --with H adds
    `#include "H"` to the check TU after common.h (a header the core chain does not reach;
    default DEFAULT_WITH, libretro-common's streams/file_stream.h for RFILE).
    The per-flavour maxima of the core copies (asserted + UNCHECKED) and of UNCHECKED live in
    abi_asserts.ratchet; flavours_gate.py ratchet checks the committed headers against them
    (they may only fall).
 4. Constants: VRAM_DIRTY_SHIFT/PAGES against gba_memory.h; MER_INPUT_US, MER_RETIRE_US,
    MER_MISS_LIMIT and VRAM_DIRTY_PAGES against tools/test_ff_pipeline.c's copies.  All six
    are required: one that is not found is a FAILURE (exit 1), never a silent drop.
Nothing is wired into the emulator build: the header is generated into --out and only
compiled by this tool's own check TU.

THE SPLIT TREE (PLAN-P-beta5 P4; P-review 2026-11-08).  After the unity split psp/main_psp.c
is a shell that includes psp/fe/X.inc.c.  The -E view already follows the includes (its
linemarkers name each fragment, aec_cparse.load_pp), and every cite is the file and line that
hold the copy: `fe/X.inc.c:N`, or `main_psp.c:N` for the shell (and for a single-file tree, so
01067e74 output is unchanged).  The raw view the constants are read from follows the shell's
`#include "fe/..."` lines the same way (read_constants -> tu_lines), so the six constant asserts
hold on a split shell too, and a constant not found fails gen (`selftest` proves both without
docker, with the raw-shell read of the pre-review tool as its mutant).  --frag FILE fe/X.inc.c
bind-mounts a MUTATED copy of one fragment over the tree's, for -E and for the constants alike.

THE COPY SITES NO T14 FLAVOUR COMPILES (P-review r3, B6).  `unbuilt` reads main_psp.c's TU
raw (aec_cparse.load_raw: the shell with each `#include "fe/X.inc.c"` expanded, comments and
directives dropped, EVERY #if arm kept, nothing expanded) through the same extract() as the
-E view, and prints the copy sites that no generated/<flavour>/fe_abi_asserts.h cites (as an
asserted copy or an UNCHECKED row), each with its #if stack.  A file-scope statement whose
name is its first token (`PSP_MODULE_INFO(...);`) is a macro call, not a declaration (C99 has
no implicit int), so it is not a site.  It REFUSES (exit 1) when the scan cannot be trusted:
a site a header cites that the raw scan does not find (other than a copy the preprocessor
expands from a macro call at that line, like PSP_MODULE_INFO's __lib_* externs), or an
uncited site in no #if group (every flavour compiles it: the headers are stale).  No docker.

Usage:
  abi_asserts.py gen --flavour-dir RUN/<fl> --src GPSP --out DIR [--main-c F] [--frag F fe/X.inc.c]...
        [--core-h H TREEPATH] [--test-c F] [--with H]... [--platform FILE]
        -> DIR/fe_abi_asserts.h, DIR/abi_check.c, DIR/report.txt; exit 1 if it does not compile
  abi_asserts.py all --run RUN --src GPSP --out DIR [--with H]...   (release, lab, harness)
  abi_asserts.py unbuilt --src GPSP [--gen DIR]          (DIR default: this tree's generated/)
  abi_asserts.py selftest [--src GPSP] [--gen DIR]       (no docker: the six constants read through
        the fragments, the committed headers' constant blocks, the raw-shell mutant; P-tidy F2)
Exit: 0 generated and compiles (unbuilt: listed; selftest: PASS), 1 an assertion or compile failure
(or a constant missing; unbuilt: refused; selftest: FAILED), 2 usage.
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
    # externs of variables main_psp.c defines itself are not copies.  Every SITE is its
    # own copy (P0a T14, PLAN-P-beta5 C12): a token-identical copy at another line is
    # asserted too, so a second copy (gpsp_rom_load_progress's file-scope one before
    # gba_reload_note beside the block-scope one in main) is checked where it is, not
    # folded into the first; only the same site seen twice is dropped.
    out = []
    seen = set()
    for r in res:
        if r[0] in defined or r[0] in own_vars:
            continue
        key = (r[0], r[1], tuple(x.text for x in r[2]))
        if key in seen:
            continue
        seen.add(key)
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


PLATFORM = os.path.join(HERE, 'abi_asserts.platform')
# The headers the check TU includes after common.h when --with is not given (C6-2): core_fe.h names
# gamepak_file_large as `struct RFILE *`, and the frontend copies say RFILE, libretro-common's typedef.
DEFAULT_WITH = ['streams/file_stream.h']


def load_platform(path=PLATFORM):
    """{name: reason} of abi_asserts.platform (name <TAB> reason; # comments)."""
    out = {}
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            p = line.rstrip('\n').split('\t')
            out[p[0]] = p[1] if len(p) > 1 else ''
    return out


def counts_of(text):
    """(asserted, unchecked, platform) as a generated header's `checked:` line states them."""
    m = re.search(r'checked: (\d+) declarations, \d+ constants; unchecked: (\d+)(?:; platform: (\d+))?', text)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)


def known_label(path=KNOWN):
    """where the reasons' line cites come from: abi_asserts.known's header says 'known
    mismatches at REV' (its cites are REV's main_psp.c lines, not this tree's)."""
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            m = re.match(r'#.*\bknown mismatches at (\w+)', line)
            if m:
                return 'its line cites are main_psp.c at %s' % m.group(1)
    return 'its line cites are of the tree it was written on'


def tname(name, nth=1):
    """the typedef of a name's nth copy (abi_fe_t_NAME, then abi_fe_t_NAME__2, ...)"""
    return 'abi_fe_t_' + name + ('' if nth == 1 else '__%d' % nth)


def typedef_of(name, d, norm=None, nth=1):
    toks = [t for t in d if t.text not in ('extern',)]
    k = next(i for i, t in enumerate(toks) if t.text == name)
    txt = []
    for i, t in enumerate(toks):
        txt.append(tname(name, nth) if i == k else t.text)
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


def cite(line):
    """the file:line of a copy: an aec_cparse.FLine (a fragment's line) prints as fe/X.inc.c:N,
    a plain int is a line of main_psp.c (the shell, or the single file)."""
    return str(line) if isinstance(line, C.FLine) else 'main_psp.c:%d' % line


FE_INC = re.compile(r'^\s*#\s*include\s+"(fe/[A-Za-z0-9_.]+)"')
LOCAL_INC = re.compile(r'^\s*#\s*include\s+"([A-Za-z0-9_.]+)"')


def file_lines(path, label):
    return [(label, i, ln) for i, ln in
            enumerate(open(path, encoding='utf-8', errors='replace').read().split('\n'), 1)]


def tu_lines(main_raw, psp_dir, over=None, _label='main_psp.c', _depth=0):
    """[(file, line, text)] of main_psp.c's own text in TU order: the shell (or single file),
    with every `#include "fe/X"` line followed by fe/X's lines (a fragment fe/X.inc.c, or
    fe/fe_priv.h and what it includes from psp/fe/).  over = {'fe/X': mutated copy}."""
    over = over or {}
    out = []
    for lab, i, ln in file_lines(main_raw, _label):
        out.append((lab, i, ln))
        m = FE_INC.match(ln)
        tgt = m.group(1) if m else None
        if tgt is None and _label.startswith('fe/'):
            m = LOCAL_INC.match(ln)
            if m and (('fe/' + m.group(1)) in over or os.path.exists(os.path.join(psp_dir, 'fe', m.group(1)))):
                tgt = 'fe/' + m.group(1)
        if tgt and _depth < 4:
            p = over.get(tgt) or os.path.join(psp_dir, tgt)
            if not os.path.exists(p):
                raise SystemExit('abi_asserts: %s:%d includes "%s", which is missing (%s)' % (lab, i, tgt, p))
            out += tu_lines(p, psp_dir, over, tgt, _depth + 1)
    return out


def macro_values(lines, names):
    """{name: (value, 'file:line')} of the FIRST #define of each name in lines [(file, line, text)]."""
    vals = {}
    for lab, i, line in lines:
        m = re.match(r'\s*#\s*define\s+(\w+)\s+(.*?)\s*(/\*.*)?$', line)
        if m and m.group(1) in names and m.group(1) not in vals:
            vals[m.group(1)] = (re.sub(r'//.*', '', m.group(2)).strip(), '%s:%d' % (lab, i))
    return vals


def prefixed(expr, names, pre):
    return re.sub(r'\b(%s)\b' % '|'.join(map(re.escape, names)), lambda m: pre + m.group(1), expr)


def read_constants(src, main_c=None, over=None, test_c=None):
    """The six constants of 4. (module docstring), as gen asserts them.
    -> (macros_fe {name: (value, cite)}, macros_test {name: (value, cite, fe value, fe cite)},
        missing [why], nfe: the fragments the TU text followed).
    main_psp.c's side is read from the TU's own text in TU order (tu_lines: the shell, or the
    single file, with every `#include "fe/..."` followed), so on a split shell the #defines that
    sit in a fragment are found and cited there.  A constant that is not found is in `missing`
    and gen FAILS on it (P-review 2026-11-08: a raw read of the split shell alone saw no fragment,
    lost all six asserts and still exited 0).  No docker: `abi_asserts.py selftest` runs it."""
    main_raw = main_c or os.path.join(src, 'psp', 'main_psp.c')
    tu = tu_lines(main_raw, os.path.join(src, 'psp'), over)
    nfe = len({lab for lab, _, _ in tu if lab.endswith('.inc.c')})
    mfe = macro_values(tu, set(CORE_MACROS) | set(TEST_MACROS))
    macros_fe = {k: mfe[k] for k in CORE_MACROS if k in mfe}
    tvals = macro_values(file_lines(test_c or os.path.join(src, TEST_FILE), TEST_FILE), set(TEST_MACROS))
    macros_test = {k: (tvals[k][0], tvals[k][1], mfe[k][0], mfe[k][1]) for k in TEST_MACROS
                   if k in tvals and k in mfe}
    missing = (['%s (not #defined in main_psp.c\'s TU text)' % k for k in CORE_MACROS + TEST_MACROS if k not in mfe]
               + ['%s (not #defined in %s)' % (k, TEST_FILE) for k in TEST_MACROS if k not in tvals])
    return macros_fe, macros_test, missing, nfe


def constants_line(macros_fe, macros_test, missing):
    """gen's report line: 'constants: all 6 found (NAME CITE, ...)' in the TU, or CONSTANT MISSING."""
    if missing:
        return 'constants: CONSTANT MISSING: ' + '; '.join(missing)
    at = {k: v[1] for k, v in macros_fe.items()}
    at.update({k: v[3] for k, v in macros_test.items() if k not in at})
    return 'constants: all %d found (%s)' % (len(CORE_MACROS) + len(TEST_MACROS), ', '.join(
        '%s %s' % (k, at[k]) for k in CORE_MACROS + [t for t in TEST_MACROS if t not in CORE_MACROS]))


def gen_verdict(rc, pin_fail, missing):
    """gen's exit status: 0 only when the check TU compiles, no pinned name is lost and all six
    constants were found (a missing constant is a failure, never a silent drop)."""
    return 0 if rc == 0 and not pin_fail and not missing else 1


# ------------------------------------------------------------- generation
def render(flavour, items, unchecked, macros_fe, macros_test, src_desc, known=None, known_at='', platform=None):
    known = known or {}
    platform = platform or []
    L = ['/* fe_abi_asserts.h -- GENERATED by gpsp/tools/refactor/abi_asserts.py; do not edit.',
         ' * Flavour: %s.  Source: %s.' % (flavour, src_desc),
         ' * Include AFTER the core headers (common.h) in a TU compiled with the core flags.',
         ' * Each assert compares a hand-copied declaration of main_psp.c\'s TU with the core\'s own.',
         ' * Cites are file:line of the tree this was generated on: main_psp.c:N is the shell (or',
         ' * the single file), fe/X.inc.c:N a fragment the shell includes (psp/fe/README.md).',
         ' * checked: %d declarations, %d constants; unchecked: %d; platform: %d (listed at the end). */' % (
             len(items), len(macros_fe) + len(macros_test), len(unchecked), len(platform)),
         '#ifndef FE_ABI_ASSERTS_H', '#define FE_ABI_ASSERTS_H', '']
    nth = {}
    for name, line, d, kind in items:
        kn = known.get(name)
        nth[name] = nth.get(name, 0) + 1
        L.append('/* %s (%s)%s */' % (cite(line), kind, (' KNOWN, normalised %s (abi_asserts.known reason, %s): %s'
                                                         % (kn[0], known_at, kn[1])) if kn else ''))
        L.append(typedef_of(name, d, kn[0] if kn else None, nth[name]))
        L.append('_Static_assert(__builtin_types_compatible_p(__typeof__(&%s), %s *),\n'
                 '               "%s %s: the frontend copy differs from the core declaration");'
                 % (name, tname(name, nth[name]), cite(line), name))
    if macros_fe:
        L.append('')
        L.append('/* constants copied from the core (r2 H-8) */')
        names = list(macros_fe)
        for nm, (val, at) in macros_fe.items():
            L.append('#define ABI_FE_%s (%s)   /* %s */' % (nm, prefixed(val, names, 'ABI_FE_'), at))
        for nm, (val, at) in macros_fe.items():
            L.append('_Static_assert(ABI_FE_%s == (%s), "%s %s differs from the core header");'
                     % (nm, nm, at, nm))
    if macros_test:
        L.append('')
        L.append('/* constants copied again by %s (r2 H-8) */' % TEST_FILE)
        for nm, (val, at, feval, feat) in macros_test.items():
            L.append('#define ABI_TEST_%s (%s)   /* %s */' % (nm, val, at))
            L.append('#define ABI_FET_%s (%s)   /* %s */' % (
                nm, prefixed(feval, list(macros_fe) + [nm], 'ABI_FE_') if nm in macros_fe else
                prefixed(feval, [], ''), feat))
            L.append('_Static_assert(ABI_FET_%s == ABI_TEST_%s, "%s %s differs from %s");'
                     % (nm, nm, at, nm, feat))
    L.append('')
    L.append('/* the sleep-quiesce stop protocol (r1 H-SQ, PLAN §1.4): audio_stop / io_thread_stop')
    L.append(' * return 0 = joined, 1 = none, which must stay SQ_DONE / SQ_NONE (psp/sleep_q.h:37-38) */')
    L.append('#ifndef SQ_DONE')
    L.append('#error "fe_abi_asserts.h: include psp/sleep_q.h first"')
    L.append('#endif')
    L.append('_Static_assert(SQ_DONE == 0 && SQ_NONE == 1, "sleep_q.h: SQ_DONE/SQ_NONE are not 0/1");')
    L.append('')
    L.append('/* UNCHECKED (%d): every copy of this flavour\'s -E that is NOT asserted: its name is not declared' % len(unchecked))
    L.append(' * by the check TU\'s headers (abi_check.c includes only common.h\'s chain and psp/sleep_q.h), or')
    L.append(' * its type is frontend-only.  A copy in an #if arm this flavour does not compile is in neither list')
    L.append(' * (abi_asserts.py unbuilt lists the copy sites that no T14 flavour compiles).')
    for name, line, why in unchecked:
        L.append(' *   %-24s %-36s %s' % (cite(line), name, why))
    L.append(' */')
    L.append('')
    L.append('/* PLATFORM (%d): copies of names that are not core ABI (abi_asserts.platform gives each reason):' % len(platform))
    L.append(' * linker symbols, the PSP_MODULE_INFO expansion, an SDK function its header lacks.  Not asserted.')
    for name, line, why in platform:
        L.append(' *   %-24s %-36s %s' % (cite(line), name, why))
    L.append(' */')
    L.append('#endif')
    return '\n'.join(L) + '\n'


def _plat_split(unchecked, plat, macros_fe, macros_test, desc, known, kat):
    """render()'s arguments after items: UNCHECKED without the platform names, then the rest,
    with the platform rows (reason from abi_asserts.platform) last."""
    return ([u for u in unchecked if u[0] not in plat], macros_fe, macros_test, desc, known, kat,
            [(n, l, plat[n]) for n, l, _ in unchecked if n in plat])


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
        pin_path=PIN, frags=None, with_h=None, platform_path=PLATFORM):
    """frags: [(mutated copy, 'fe/X.inc.c')] laid over the tree's fragments (-E and constants).
    with_h: tree-relative headers the check TU includes after common.h (C6-1 --with)."""
    os.makedirs(outdir, exist_ok=True)
    known = load_known(known_path)
    plat = load_platform(platform_path)
    over = {}
    for host, rel in (frags or []):
        if not re.match(r'^fe/[A-Za-z0-9_.]+$', rel) or not os.path.exists(host):
            raise SystemExit('abi_asserts: --frag %s %s: want an existing file and fe/<name>' % (host, rel))
        over[rel] = host
    ip = os.path.join(outdir, 'main_psp.i')
    with open(ip, 'w', newline='\n') as f:
        f.write(P.preprocess(src, flavour_dir, override=main_c, extra=[(h, r) for r, h in sorted(over.items())]))
    toks = C.load_pp(ip)
    items = extract(toks)
    unchecked = []
    macros_fe, macros_test, missing, nfe = read_constants(src, main_c, over, test_c)
    with open(os.path.join(outdir, 'abi_check.c'), 'w', newline='\n') as f:
        f.write('/* T14 check TU: the core header chain, then the generated asserts. */\n'
                '#include "common.h"\n' + ''.join('#include "%s"\n' % h for h in (with_h or [])) +
                '#include "psp/sleep_q.h"\n#include "fe_abi_asserts.h"\n')
    hp = os.path.join(outdir, 'fe_abi_asserts.h')
    muts = (['MUTANT %s' % os.path.basename(main_c)] if main_c else []) + \
        ['MUTANT %s=%s' % (r, os.path.basename(h)) for r, h in sorted(over.items())]
    desc = ('psp/main_psp.c%s%s via psp-gcc -E' % (
        ' and the %d fragments (fe/X.inc.c) it includes' % nfe if nfe else '', (' (%s)' % ', '.join(muts)) if muts else ''))
    kat = known_label(known_path)
    # discovery: move to UNCHECKED the items whose typedef or assert does not compile in the check
    # TU: a name its headers (common.h's chain, psp/sleep_q.h) do not declare, or a frontend-only
    # type.  A failed static assertion is a real mismatch and is never moved.
    for rnd in range(6):
        with open(hp, 'w', newline='\n') as f:
            f.write(render(flavour, items, *_plat_split(unchecked, plat, macros_fe, macros_test, desc, known, kat)))
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
            mm = re.search(r'abi_fe_t_(\w+?)(?:__\d+)?\b', txt) or re.search(r'__typeof__\(&(\w+)\)', txt)
            if not mm and ln >= 2:
                mm = re.search(r'__typeof__\(&(\w+)\)', hl[ln - 2])
            if mm:
                nm = mm.group(1)
                why = ('not declared by the check TU\'s headers' if ('undeclared' in msg and "'%s'" % nm in msg)
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
        f.write(render(flavour, items, *_plat_split(unchecked, plat, macros_fe, macros_test, desc, known, kat)))
    rc, so, se = compile_check(src, flavour_dir, outdir, core_h=core_h)
    fails = [l for l in se.split('\n') if 'error' in l]
    platform = [u for u in unchecked if u[0] in plat]
    unchecked = [u for u in unchecked if u[0] not in plat]
    multi = sorted({n for n, _, _, _ in items if sum(1 for x in items if x[0] == n) > 1})
    rep = ['T14 %s: %d copied declarations found, %d asserted, %d unchecked, %d platform; %d constants asserted'
           % (flavour, len(items) + len(unchecked) + len(platform), len(items), len(unchecked), len(platform),
              len(macros_fe) + len(macros_test)),
           'copies asserted at more than one site: %s' % (', '.join(
               '%s (%s)' % (n, ', '.join(cite(x[1]) for x in items if x[0] == n)) for n in multi)
               or 'none'),
           'compile (psp-gcc -fsyntax-only, core flags): %s' % ('ok' if rc == 0 else 'FAILED'),
           constants_line(macros_fe, macros_test, missing),
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
    return gen_verdict(rc, pin_fail, missing)


# ------------------------------------------------------------- the sites no flavour compiles
HDR_CITE = r'((?:main_psp\.c|fe/[A-Za-z0-9_.]+):\d+)'
MACRO_CALL = re.compile(r'^\s*([A-Z_][A-Z0-9_]*)\s*\(')


class Unbuilt(Exception):
    pass


def raw_sites(src, main_c=None):
    """[(cite, name, kind, ifs)] of every copy site of main_psp.c's TU in ANY #if arm, in TU order:
    the raw view (aec_cparse.load_raw, follow=True) through extract().  ifs = the #if stack of the
    site's name token (load_raw's form: '#if X', '#if X | #else', outermost first)."""
    toks = C.load_raw(main_c or os.path.join(src, 'psp', 'main_psp.c'), follow=True)
    out = []
    for name, line, d, kind in extract(toks):
        if kind == 'proto' and d and d[0].text == name:
            continue                         # NAME(...); at file scope: a macro call, not a declaration
        nt = next(t for t in d if t.text == name and t.line == line)
        out.append((cite(line), name, kind, nt.ifs))
    return out


def header_sites(path):
    """{(cite, name)} that one generated fe_abi_asserts.h names: each asserted copy (its
    `/* CITE (kind) */` line before typedef abi_fe_t_NAME) and each UNCHECKED row."""
    if not os.path.exists(path):
        raise Unbuilt('no %s (regenerate T14: abi_asserts.py all)' % path)
    hl = open(path, encoding='utf-8').read().split('\n')
    out = set()
    for k, ln in enumerate(hl):
        m = re.match(r'^/\* %s \((?:extern|block|proto)\)' % HDR_CITE, ln)
        if m and k + 1 < len(hl):
            mm = re.search(r'\babi_fe_t_(\w+?)(?:__\d+)?\b', hl[k + 1])
            if not mm:
                raise Unbuilt('%s:%d: a cite with no typedef after it' % (path, k + 1))
            out.add((m.group(1), mm.group(1)))
            continue
        m = re.match(r'^ \*   %s\s+(\w+)\s' % HDR_CITE, ln)
        if m:
            out.add((m.group(1), m.group(2)))
    if not out:
        raise Unbuilt('%s names no copy' % path)
    return out


def ifs_text(ifs):
    """load_raw's #if stack as text: '#ifdef A > #else after #if B'"""
    out = []
    for fr in ifs:
        arms = fr.split(' | ')
        out.append(arms[0] if len(arms) == 1 else '%s after %s' % (arms[-1], ', '.join(arms[:-1])))
    return ' > '.join(out)


def unbuilt(src, headers, main_c=None):
    """(uncited sites [(cite, name, kind, ifs)], {'raw': n, 'cited': n, 'macro': [(cite, name)]}).
    Raises Unbuilt when the raw scan cannot be trusted (module docstring)."""
    sites = raw_sites(src, main_c)
    cited = set()
    for h in headers:
        cited |= header_sites(h)
    raw = {(c, n) for c, n, _, _ in sites}
    tu = {'%s:%d' % (lab, i): ln for lab, i, ln in tu_lines(main_c or os.path.join(src, 'psp', 'main_psp.c'),
                                                           os.path.join(src, 'psp'))}
    macro, lost = [], []
    for c, n in sorted(cited - raw):
        m = MACRO_CALL.match(tu.get(c, ''))
        (macro if m and m.group(1) != n else lost).append((c, n))
    if lost:
        raise Unbuilt('%d site(s) a generated header cites are not in the raw scan of the TU (%s): the scan '
                      'cannot be trusted, or the headers are of another tree (regenerate: abi_asserts.py all)'
                      % (len(lost), ', '.join('%s %s' % x for x in lost[:8])))
    unc = [s for s in sites if (s[0], s[1]) not in cited]
    every = [s for s in unc if not s[3]]
    if every:
        raise Unbuilt('%d copy site(s) in no #if group are cited by no generated header (%s): every flavour '
                      'compiles them, so the headers are stale (regenerate: abi_asserts.py all)'
                      % (len(every), ', '.join('%s %s' % (c, n) for c, n, _, _ in every[:8])))
    return unc, {'raw': len(sites), 'cited': len(raw & cited), 'macro': macro}


def cmd_unbuilt(src, gen_dir):
    headers = [os.path.join(gen_dir, fl, 'fe_abi_asserts.h') for fl in FLAVOURS]
    try:
        unc, st = unbuilt(src, headers)
    except (Unbuilt, ValueError) as e:
        print('abi_asserts unbuilt: REFUSED: %s' % e, file=sys.stderr)
        return 1
    def shown(h):
        r = os.path.relpath(h, src).replace('\\', '/')
        return h if r.startswith('..') else r
    print('T14 unbuilt: %d copy sites in the raw TU (every #if arm); %d cited by %s (asserted or UNCHECKED); '
          '%d cited copies come from a macro call at their line (%s); %d sites in no T14 flavour:'
          % (st['raw'], st['cited'], ', '.join(shown(h) for h in headers),
             len(st['macro']), ', '.join('%s %s' % x for x in st['macro']) or 'none', len(unc)))
    for c, n, k, ifs in unc:
        print('  %-24s %-28s %-6s %s' % (c, n, k, ifs_text(ifs)))
    return 0


# ------------------------------------------------------------- selftest (no docker)
CONST_LINE = re.compile(r'^(?:#define ABI_(?:FE|TEST|FET)_\w+ |_Static_assert\(ABI_)')
SPLIT_INC = re.compile(r'^\s*#\s*include\s+"fe/[A-Za-z0-9_.]+\.inc\.c"')


def _cite_holds(src, cite, name, main_c=None):
    """the cited file:line (main_psp.c:N = the shell, fe/X:N = psp/fe/X, TEST_FILE:N) #defines name."""
    f, n = cite.rsplit(':', 1)
    p = (main_c or os.path.join(src, 'psp', 'main_psp.c')) if f == 'main_psp.c' else \
        os.path.join(src, TEST_FILE) if f == TEST_FILE else os.path.join(src, 'psp', f)
    if not os.path.exists(p):
        return False
    ls = open(p, encoding='utf-8', errors='replace').read().split('\n')
    return 0 < int(n) <= len(ls) and re.match(r'\s*#\s*define\s+%s\b' % re.escape(name), ls[int(n) - 1]) is not None


def selftest(src, gen_dir):
    """read_constants and gen's verdict on it, with no docker (P-tidy F2):
      C1 this tree: all six found, each cite names a line that #defines it; on a split shell
         the TU text followed its fragments
      C2 each committed generated/<flavour>/fe_abi_asserts.h: its constant block is exactly what
         read_constants gives on this tree (values and cites), its header says 6 constants, and
         it has declarations + 6 + 1 (sleep_q) _Static_asserts
      M1 the pre-review rule (the shell's own text, no include followed): on a split shell all six
         are missing and gen fails (1); on a single file nothing changes
      C3, M2-M4 a synthetic split tree: found through a fragment and an fe/ header (fe/X:N cites);
         a #define deleted -> named in missing, gen fails; a --frag copy is read instead of the
         tree's fragment; an include of a missing fragment is refused
      C4 a synthetic single file: main_psp.c:N cites and 0 fragments (01067e74's case)"""
    import shutil
    import tempfile
    bad = [0]

    def t(cond, msg):
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', msg))
        bad[0] += 0 if cond else 1
    names = CORE_MACROS + [k for k in TEST_MACROS if k not in CORE_MACROS]
    main_raw = os.path.join(src, 'psp', 'main_psp.c')
    if not os.path.exists(main_raw):
        print('abi_asserts selftest: no %s (pass --src GPSP)' % main_raw)
        return 2
    split = any(SPLIT_INC.match(l) for l in open(main_raw, encoding='utf-8', errors='replace'))
    mf, mt, miss, nfe = read_constants(src)
    cites = [(k, v[1]) for k, v in mf.items()] + [(k, c) for k, (_, c0, _, c) in mt.items()] + \
        [(k, c0) for k, (_, c0, _, _) in mt.items()]
    t(not miss and len(mf) + len(mt) == 6 and gen_verdict(0, 0, miss) == 0,
      'C1 this tree (%s shell, %d fragments followed): %d constants found (%s)%s'
      % ('a split' if split else 'a single-file', nfe, len(mf) + len(mt),
         ', '.join('%s %s' % (k, (mf.get(k) or mt[k][2:])[1] if k in mf or k in mt else '-') for k in names),
         '; MISSING ' + '; '.join(miss) if miss else ''))
    cl = constants_line(mf, mt, miss)
    t(cl.startswith('constants: all 6 found (') and all(
        ('%s %s' % (k, c)) in cl for k, c in [(k, v[1]) for k, v in mf.items()] + [(k, v[3]) for k, v in mt.items()]),
      'C1 gen\'s report line: %s' % cl)
    held = [c for k, c in cites if not _cite_holds(src, c, k)]
    t(cites and not held and (nfe > 0) == split,
      'C1 every constant cite names a line that #defines it (%d cites%s)'
      % (len(cites), '; NOT: ' + ', '.join(held) if held else ''))
    def header_ok(txt, fl):
        want = [l for l in render(fl, [], [], mf, mt, '').split('\n') if CONST_LINE.match(l)]
        got = [l for l in txt.split('\n') if CONST_LINE.match(l)]
        m = re.search(r'checked: (\d+) declarations, (\d+) constants', txt)
        n_sa = txt.count('_Static_assert(')
        ok = bool(want) and got == want and bool(m) and int(m.group(2)) == 6 and n_sa == int(m.group(1)) + 6 + 1
        return ok, ('%d constant lines%s, header "%s", %d _Static_asserts' % (
            len(got), ' = this tree\'s' if got == want else ' (DIFFER from the %d wanted)' % len(want),
            m.group(0) if m else 'no "checked:" line', n_sa))
    rel = None
    for fl in FLAVOURS:
        h = os.path.join(gen_dir, fl, 'fe_abi_asserts.h')
        if not os.path.exists(h):
            t(False, 'C2 [%s] no %s' % (fl, h))
            continue
        txt = open(h, encoding='utf-8').read()
        rel = rel or txt
        ok, msg = header_ok(txt, fl)
        t(ok, 'C2 [%s] committed header: %s' % (fl, msg))
    if rel:
        # M5: the header the pre-review tool wrote on the split shell ("0 constants", 55 asserts in release)
        old = '\n'.join(l for l in rel.split('\n') if not CONST_LINE.match(l))
        old = re.sub(r'(checked: \d+ declarations, )\d+( constants)', r'\g<1>0\2', old)
        ok, msg = header_ok(old, FLAVOURS[0])
        t(not ok, 'M5 a header without the constant block (the reviewed 55-vs-61 case) fails C2: %s' % msg)
    # M1: the pre-review rule, the shell's own text only
    g = globals()
    real = g['tu_lines']
    g['tu_lines'] = lambda main_raw, psp_dir, over=None, *a, **k: file_lines(main_raw, 'main_psp.c')
    try:
        _, _, miss1, nfe1 = read_constants(src)
    finally:
        g['tu_lines'] = real
    if split:
        t(len(miss1) == 6 and nfe1 == 0 and gen_verdict(0, 0, miss1) == 1
          and constants_line({}, {}, miss1).startswith('constants: CONSTANT MISSING: '),
          'M1 the raw shell alone (the pre-review read): %d of 6 constants missing, gen exits %d (a FAIL, '
          'not "0 constants asserted" and exit 0)' % (len(miss1), gen_verdict(0, 0, miss1)))
    else:
        t(miss1 == miss, 'M1 the raw read on a single file: same constants (%d missing)' % len(miss1))
    # synthetic trees
    d = tempfile.mkdtemp(prefix='abi_selftest.')
    try:
        def w(rel, text):
            p = os.path.join(d, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'w', newline='\n') as f:
                f.write(text)
            return p
        frag = ('/* a fragment */\n#define VRAM_DIRTY_PAGES ((1024 * 96) >> VRAM_DIRTY_SHIFT)\n'
                '#define MER_RETIRE_US     9000\n#define MER_INPUT_US      50000\n#define MER_MISS_LIMIT    120\n')
        w('psp/main_psp.c', '/* shell */\n#include "fe/fe_priv.h"\n#include "fe/a.inc.c"\nint main(void) { return 0; }\n')
        w('psp/fe/fe_priv.h', '#ifndef FE_PRIV_H\n#define FE_PRIV_H\n#define VRAM_DIRTY_SHIFT 10\n#endif\n')
        fa = w('psp/fe/a.inc.c', frag)
        w(TEST_FILE, '#define VRAM_DIRTY_PAGES 96\n#define MER_INPUT_US 50000\n#define MER_RETIRE_US 9000\n'
                     '#define MER_MISS_LIMIT 120\n')
        mf3, mt3, miss3, nfe3 = read_constants(d)
        t(not miss3 and nfe3 == 1 and mf3.get('VRAM_DIRTY_SHIFT', ('', ''))[1] == 'fe/fe_priv.h:3'
          and mf3.get('VRAM_DIRTY_PAGES', ('', ''))[1] == 'fe/a.inc.c:2' and mt3.get('MER_MISS_LIMIT', ('',) * 4)[3] == 'fe/a.inc.c:5',
          'C3 synthetic split shell: found through fe/fe_priv.h and fe/a.inc.c (%s)'
          % ', '.join('%s %s' % (k, v[1]) for k, v in sorted(mf3.items())))
        w('psp/fe/a.inc.c', frag.replace('#define MER_MISS_LIMIT    120\n', ''))
        _, _, miss4, _ = read_constants(d)
        t(len(miss4) == 1 and miss4[0].startswith('MER_MISS_LIMIT ') and gen_verdict(0, 0, miss4) == 1,
          'M2 MER_MISS_LIMIT deleted from the fragment: missing %s, gen exits %d' % (miss4, gen_verdict(0, 0, miss4)))
        mut = w('mut/a.inc.c', frag.replace('MER_INPUT_US      50000', 'MER_INPUT_US      40000'))
        _, mt5, miss5, _ = read_constants(d, over={'fe/a.inc.c': mut})
        t(not miss5 and mt5.get('MER_INPUT_US', ('',) * 4)[2] == '40000',
          'M3 --frag fe/a.inc.c (a mutated copy) is read instead of the tree\'s: MER_INPUT_US %s'
          % mt5.get('MER_INPUT_US', ('',) * 4)[2])
        w('psp/main_psp.c', '/* shell */\n#include "fe/fe_priv.h"\n#include "fe/a.inc.c"\n#include "fe/gone.inc.c"\n')
        try:
            read_constants(d)
            t(False, 'M4 an include of a missing fragment must be refused')
        except SystemExit as ex:
            t('fe/gone.inc.c' in str(ex), 'M4 an include of a missing fragment is refused: %s' % ex)
        os.remove(fa)
        w('psp/main_psp.c', '/* single file */\n' + open(os.path.join(d, 'psp/fe/fe_priv.h')).read() + frag)
        mf6, mt6, miss6, nfe6 = read_constants(d)
        t(not miss6 and nfe6 == 0 and mf6['VRAM_DIRTY_SHIFT'][1] == 'main_psp.c:4',
          'C4 synthetic single file: %d found, 0 fragments, VRAM_DIRTY_SHIFT %s'
          % (len(mf6) + len(mt6), mf6.get('VRAM_DIRTY_SHIFT', ('', '-'))[1]))
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('abi_asserts selftest: %s' % ('PASS' if not bad[0] else '%d FAILED' % bad[0]))
    return 1 if bad[0] else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    g = sub.add_parser('gen')
    g.add_argument('--flavour-dir', required=True)
    g.add_argument('--flavour')
    g.add_argument('--src', required=True)
    g.add_argument('--out', required=True)
    g.add_argument('--main-c', help='a mutated copy of psp/main_psp.c (bind-mounted over the tree)')
    g.add_argument('--frag', nargs=2, action='append', metavar=('FILE', 'fe/X.inc.c'),
                   help='a mutated copy of one fragment of a split main_psp.c (bind-mounted over psp/fe/X.inc.c '
                        'for -E, and read for the constants); repeatable')
    g.add_argument('--core-h', nargs=2, metavar=('FILE', 'TREEPATH'),
                   help='a mutated copy of a core header, e.g. /tmp/main.h main.h')
    g.add_argument('--test-c', help='a mutated copy of tools/test_ff_pipeline.c')
    g.add_argument('--with', dest='with_h', action='append', metavar='H',
                   help='a tree-relative header the check TU includes after common.h (repeatable)')
    g.add_argument('--platform', default=PLATFORM, help='platform names (abi_asserts.platform)')
    g.add_argument('--known', default=KNOWN)
    g.add_argument('--pin', default=PIN, help='pinned asserted names (abi_asserts.pinned)')
    g.add_argument('--write-pin', action='store_true', help='rewrite this flavour\'s pin (after review)')
    a = sub.add_parser('all')
    a.add_argument('--run', required=True)
    a.add_argument('--src', required=True)
    a.add_argument('--out', required=True)
    a.add_argument('--write-pin', action='store_true')
    a.add_argument('--with', dest='with_h', action='append', metavar='H')
    u = sub.add_parser('unbuilt')
    u.add_argument('--src', required=True)
    u.add_argument('--gen', default=os.path.join(HERE, 'generated'),
                   help='DIR holding <flavour>/fe_abi_asserts.h (default: this tree\'s generated/)')
    s = sub.add_parser('selftest', help='the constant read and gen\'s verdict on it, no docker')
    s.add_argument('--src', default=os.path.normpath(os.path.join(HERE, '..', '..')),
                   help='the gpsp tree (default: this tool\'s)')
    s.add_argument('--gen', default=os.path.join(HERE, 'generated'),
                   help='DIR holding <flavour>/fe_abi_asserts.h (default: this tree\'s generated/)')
    o = ap.parse_args()
    if o.cmd == 'selftest':
        return selftest(o.src, o.gen)
    if o.cmd == 'unbuilt':
        return cmd_unbuilt(o.src, o.gen)
    if o.cmd == 'gen':
        fl = o.flavour or os.path.basename(os.path.normpath(o.flavour_dir))
        return gen(o.flavour_dir, o.src, o.out, fl, o.main_c, o.core_h, o.test_c, o.known,
                   'WRITE' if o.write_pin else o.pin, [tuple(x) for x in (o.frag or [])], o.with_h or DEFAULT_WITH, o.platform)
    if o.cmd == 'all':
        rc = 0
        for fl in FLAVOURS:
            rc |= gen(os.path.join(o.run, fl), o.src, os.path.join(o.out, fl), fl,
                      pin_path='WRITE' if o.write_pin else PIN, with_h=o.with_h or DEFAULT_WITH)
        return rc
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
