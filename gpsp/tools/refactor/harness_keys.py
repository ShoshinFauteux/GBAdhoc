#!/usr/bin/env python3
"""harness_keys.py -- the harness-ini key table (tier C, C2-0; gpsp/docs/design/TIER-C.md C-2 §2.4).

A key the source never reads is silently ignored on the console (a misspelt key just keeps its default), so
the Lab refuses to stage a key it does not know.  Until now the Lab learned the keys by grepping
`fe_ini_get*(HARNESS_INI, "k"` in psp/*.c: on the split tree (psp/fe/*.inc.c) that finds 7 of
them.  This tool writes ONE table of every harness read instead, from the preprocessed source of
each flavour, and the Lab reads the table (GBAdhoc-Lab backend/lab/inikeys.py).

  tools/refactor/generated/harness_keys.tsv   (committed; regenerate with `gen`)
     key <TAB> getter <TAB> default <TAB> site <TAB> flavours
     one row per read site; `default` is the default argument as the -E has it (tokens joined by
     one space; `-` for a string read, whose default is "not found"); `site` is psp/FILE:LINE of
     the getter (a macro read such as RW_KEY is cited at its invocation line); `flavours` lists
     the T14 flavours (release, lab, harness: flavours.sh) whose -E compiles that site, or
     `opt-in:<SWITCH>` for a read only an opt-in switch compiles (raw source, no flavour builds it).

WHAT COUNTS AS A HARNESS READ (per translation unit, all from `psp-gcc -E`):
  main_psp.o    a GETTERS call whose path argument is `p_harness` (HARNESS_INI expands to it),
                RW_KEY/RW_FLAG included (they expand to fe_ini_get_int).  EVERY other use of
                `p_harness` must be classified (NOT_READS callees, its declaration, or an alias
                `NAME = ... p_harness ...;` whose reads are listed in ALIAS_KEYS); an
                unclassified use FAILS, so a new indirect reader cannot slip past the table.
                A player build (release; HARNESS_FOLD, C2-3) has no getter call: its rows are the
                folded expansions `( ( void ) sizeof ( "k" ) , ( long ) ( d ) )` (fold_at), so
                check (b) still compares its defaults with the rig flavours'.  Defaults are
                written with every nested harness read in fe_priv.h's spelling (canon).
  online_psp.o  calls whose path argument is `h` (online_psp_configure's harness path), incl. the
                get_str/get_int(h, c, "k", ...) helpers.
  lab_harness.o calls on `L_ini` (the task ini, which IS the harness ini in a Lab build) or `ini`
  lab_ffpace.o  (a task's TASK.INI / lab_ffpace_boot's harness path), task_ini_int("k", def), and
                the key list of lab_audit_keys.  Every other standalone "lab_*" literal in these
                files must be in LAB_NOT_KEYS (the old Lab grep took each one for a key).
A read whose key is not a literal FAILS unless its function is in DYNAMIC_OK (the helpers that
forward a caller's literal key).

CHECKS (gen and check; C-2.md §2.4):
  (a) every default is side-effect free: no call, no assignment (= += ...), no ++/--; the one call
      allowed is a nested read of the harness itself (it is a row of its own and folds alike);
  (b) a site compiled by several flavours has the same default text in each (ADR-0067's
      "player build = harness build without the key", as a check instead of a convention);
  (c) is ci_greps.py R7 (no raw fe_ini_get*(HARNESS_INI|p_harness ...) outside fe_priv.h; fail
      mode since C2-1 rewrote the call form to fe_priv.h's HARNESS_INT/HARNESS_STR).
  `check` (no docker) also proves the committed table matches the tree: every row's site still
  holds its key literal, and every key of the raw greps (over psp/**: HARNESS_INT/HARNESS_STR,
  the old Lab grep's raw call in both spellings, multi-line calls, RW_KEY/RW_FLAG) is in the
  table.

Usage:
  harness_keys.py gen --run RUN [--src GPSP] [--out TSV] [--pp-dir DIR]
        RUN: a flavours.sh output (RUN/<flavour>/logs/.build-eboot.log holds the compile lines);
        psp-gcc -E runs in docker (aec_pspcc).  --pp-dir keeps the .i files.
  harness_keys.py gen --from-pp DIR [--src GPSP] [--out TSV]
        the same from saved DIR/<flavour>/<obj>.i files (no docker).
  harness_keys.py check [--src GPSP] [--tsv TSV] [--expect-keys N] [--expect-sites N]
  harness_keys.py selftest
Exit: 0 ok, 1 a check failed, 2 usage.
"""
import argparse
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import aec_cparse as C  # noqa: E402

FLAVOURS = ('release', 'lab', 'harness')
DEFAULT_TSV = os.path.join(HERE, 'generated', 'harness_keys.tsv')
# translation units: object, source (relative to psp/), the names that hold the harness path
TUS = (('main_psp.o', 'main_psp.c', ('p_harness',)),
       ('online_psp.o', 'online_psp.c', ('h',)),
       ('lab_harness.o', 'lab_harness.c', ('L_ini', 'ini')),
       ('lab_ffpace.o', 'lab_ffpace.c', ('ini',)))
# getter -> (index of the path argument or None = implicit harness, key index, default index or None)
GETTERS = {'fe_ini_get_int': (0, 1, 2), 'fe_ini_get': (0, 1, None),
           'pcfg_fps_x100': (0, 1, 2),                       # fe/boot.inc.c net_session_fps
           'get_int': (0, 2, 3), 'get_str': (0, 2, None),    # online_psp.c (h, c, key, ...)
           'task_ini_int': (None, 0, 1)}                     # lab_harness.c (key, def) on L_ini
IMPLICIT_TU = {'task_ini_int': 'lab_harness.c'}
# fe_priv.h's call form (C2-1), as the RAW source spells it (opt_in_rows only; the -E has the
# getter it expands to): macro -> (the getter the table names, key index, default index or None)
MACRO_GETTERS = {'HARNESS_INT': ('fe_ini_get_int', 0, 1), 'HARNESS_STR': ('fe_ini_get', 0, None),
                 'HARNESS_FPS_X100': ('pcfg_fps_x100', 0, 1)}
# functions whose harness read takes the key from their caller (the caller's literal is the row)
DYNAMIC_OK = {('online_psp.c', 'get_str'), ('online_psp.c', 'get_int'),
              ('lab_harness.c', 'task_ini_int'), ('lab_harness.c', 'lab_audit_keys')}
# main_psp.o: callees that take p_harness without reading a key from it
NOT_READS = {'fe_ini_never': 'ADR-0067 player builds: never open it',
             'fe_ini_audit': 'unknown-key audit of the whole file',
             'fe_ini_audit_report': 'unknown-key audit report',
             'file_exists': 'have_harness',
             'snprintf': 'init_paths builds the path',
             'sizeof': 'init_paths',
             'fe_evt': 'logs the path',
             'online_psp_configure': 'reads through `h` in online_psp.o (counted there)',
             'lab_ffpace_boot': 'reads through `ini` in lab_ffpace.o (counted there)',
             'lab_boot': 'the Lab points p_harness at the task ini'}
# reads through an alias of p_harness (`src = have_variant ? VARIANT_INI : HARNESS_INI`): every
# literal in the key argument must be listed; True = a harness key, False = the variant.ini spelling
ALIAS_KEYS = {'silent': True, 'role': True, 'group': True, 'silent_wireless': False}
# standalone "lab_*" literals in lab_*.c that are NOT keys
LAB_NOT_KEYS = {'lab_wall_limit': 'an exit reason (lab_finish), not a key',
                'lab_ffpace': 'a thread name'}
LAB_KEY_RE = re.compile(r'^"lab_[a-z0-9_]+"$')
ASSIGN_OPS = {'=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=', '++', '--'}


class Fail(Exception):
    pass


# ------------------------------------------------------------- extraction (one -E view)
def split_args(toks, open_i):
    """argument token lists of the call whose '(' is toks[open_i]."""
    close = toks[open_i].pair.idx
    args, cur, i = [], [], open_i + 1
    while i < close:
        t = toks[i]
        if t.text == ',':
            args.append(cur)
            cur = []
            i += 1
            continue
        if t.text in ('(', '[', '{') and t.pair is not None:
            cur += toks[i:t.pair.idx + 1]
            i = t.pair.idx + 1
            continue
        cur.append(t)
        i += 1
    if cur or args:
        args.append(cur)
    return args


def side_effects(toks, names=('p_harness',)):
    """the reason a default expression is not side-effect free, or None (check a).  The one call
    allowed is a nested read of the harness itself (`folders`' default reads `browser`,
    fe/boot.inc.c:279): it is a row of its own and folds exactly like the read around it."""
    for k, t in enumerate(toks):
        if t.text in ASSIGN_OPS:
            return 'operator %s' % t.text
        if t.kind == 'id' and t.text not in C.KEYWORDS and k + 1 < len(toks) and toks[k + 1].text == '(':
            if (t.text in GETTERS and GETTERS[t.text][0] == 0 and k + 3 < len(toks)
                    and toks[k + 2].text in names and toks[k + 3].text == ','):
                continue
            if t.text in MACRO_GETTERS:       # raw source (opt_in_rows): fe_priv.h's call form
                continue
            return 'call %s()' % t.text
    return None


# fe_priv.h's folded call form (C2-3, HARNESS_FOLD 1 = a player build), as the -E has it:
#   HARNESS_INT(k, d)      ( ( void ) sizeof ( "k" ) , ( long ) ( d ) )
#   HARNESS_FPS_X100(k, d) ( ( void ) sizeof ( "k" ) , ( int ) ( d ) )
#   HARNESS_STR(k, o, n)   ( ( void ) sizeof ( "k" ) , ( void ) ( o ) , ( void ) ( n ) , harness_not_found ( ) )
# A player build has no harness read left to find, so these are its rows: check (b) then still
# compares its defaults with the rig flavours' (ADR-0067's no-drift guarantee survives the fold).
FOLD_HEAD = ('(', '(', 'void', ')', 'sizeof', '(', None, ')', ',')
FOLD_GETTER = {'long': ('fe_ini_get_int', 'HARNESS_INT'), 'int': ('pcfg_fps_x100', 'HARNESS_FPS_X100')}
CANON_GETTER = {'fe_ini_get_int': 'HARNESS_INT', 'pcfg_fps_x100': 'HARNESS_FPS_X100', 'fe_ini_get': 'HARNESS_STR'}


def fold_at(toks, j):
    """(getter, macro, key token, [arg token lists after the key], end index) when toks[j] opens
    a folded harness read, else None."""
    if j + len(FOLD_HEAD) >= len(toks) or toks[j].text != '(' or toks[j].pair is None:
        return None
    for k, want in enumerate(FOLD_HEAD):
        t = toks[j + k]
        if (want is None and t.kind != 'str') or (want is not None and t.text != want):
            return None
    end = toks[j].pair.idx
    rest = toks[j + len(FOLD_HEAD):end]

    def cast_paren(seq, ty):
        # `( ty ) ( X )` -> X tokens, or None
        if (len(seq) >= 5 and [x.text for x in seq[:3]] == ['(', ty, ')'] and seq[3].text == '('
                and seq[3].pair is not None and seq[3].pair.idx == seq[-1].idx):
            return seq[4:-1]
        return None
    key = toks[j + 6]
    for ty, (getter, macro) in FOLD_GETTER.items():
        d = cast_paren(rest, ty)
        if d is not None:
            return getter, macro, key, [d], end
    parts = [[]]
    for x in rest:
        if x.text == ',' and not any(p.text in '([{' for p in parts[-1]
                                     if p.pair is not None and p.pair.idx > x.idx):
            parts.append([])
        else:
            parts[-1].append(x)
    if len(parts) == 3 and [x.text for x in parts[2]] == ['harness_not_found', '(', ')']:
        o, n = cast_paren(parts[0], 'void'), cast_paren(parts[1], 'void')
        if o is not None and n is not None:
            return 'fe_ini_get', 'HARNESS_STR', key, [o, n], end
    return None


def canon(toks, seq, names=('p_harness',)):
    """the text of token list `seq` (a slice of `toks`) with every harness read in fe_priv.h's
    spelling, so a default holding a nested read reads the same folded or not (check b)."""
    out, i = [], 0
    while i < len(seq):
        t = seq[i]
        j = t.idx
        f = fold_at(toks, j)
        if f is not None:
            getter, macro, key, args, end = f
            out.append('%s ( %s , %s )' % (macro, key.text, ' , '.join(canon(toks, a, names) for a in args)))
            i += end - j + 1
            continue
        if (t.text in CANON_GETTER and j + 1 < len(toks) and toks[j + 1].text == '('
                and toks[j + 1].pair is not None):
            args = split_args(toks, j + 1)
            if len(args) >= 2 and len(args[0]) == 1 and args[0][0].text in names:
                out.append('%s ( %s )' % (CANON_GETTER[t.text], ' , '.join(canon(toks, a, names) for a in args[1:])))
                i += toks[j + 1].pair.idx - j + 1
                continue
        out.append(t.text)
        i += 1
    return ' '.join(out)


def _fn_spans(toks):
    return sorted((lo, hi, name) for name, (lo, hi) in C.functions(toks).items())


def _fn_at(spans, i):
    for lo, hi, name in spans:
        if lo <= i <= hi:
            return name
    return None


def _stmt_start(toks, i):
    j = i
    while j > 0 and toks[j - 1].text not in (';', '{', '}'):
        if toks[j - 1].text in (')', ']') and toks[j - 1].pair is not None:
            j = toks[j - 1].pair.idx
            continue
        j -= 1
    return j


def _block_of(toks, i):
    """(lo, hi) of the innermost {...} around toks[i] (the whole TU at file scope)."""
    best = (0, len(toks) - 1)
    for j in range(i, -1, -1):
        t = toks[j]
        if t.text == '{' and t.pair is not None and t.pair.idx > i:
            return (j, t.pair.idx)
    return best


def site_of(tu_src, t):
    f = t.file if t.file != 'main_psp.c' else tu_src
    return 'psp/%s:%d' % (f, int(t.line))


def extract(toks, tu_src, names):
    """rows [(key, getter, default, site)] and errors [str] of one TU's -E tokens."""
    rows, errs, used = [], [], set()
    spans = _fn_spans(toks)
    names = set(names)
    aliases = []                      # (name, lo, hi)
    strict = tu_src == 'main_psp.c'
    # aliases of the harness path (main TU): `NAME = <expr with p_harness>`
    if strict:
        for i, t in enumerate(toks):
            if t.kind != 'id' or t.text not in names:
                continue
            s = _stmt_start(toks, i)
            stmt = toks[s:i]
            eq = [k for k, x in enumerate(stmt) if x.text == '=']
            # p_harness must be the value itself (`src = c ? a : p_harness`), not a call argument
            inner = any(x.text == '(' and x.pair is not None and x.pair.idx > i for x in stmt)
            if eq and stmt[eq[0] - 1].kind == 'id' and not inner:
                lo, hi = _block_of(toks, i)
                aliases.append((stmt[eq[0] - 1].text, lo, hi))
                used.add(i)

    def alias_at(name, i):
        return any(a == name and lo <= i <= hi for a, lo, hi in aliases)

    for i, t in enumerate(toks):
        if t.kind != 'id' or i + 1 >= len(toks) or toks[i + 1].text != '(' or toks[i + 1].pair is None:
            continue
        callee = t.text
        if i and toks[i - 1].kind == 'id' and toks[i - 1].text != 'return':
            continue                      # a declaration or definition (`int task_ini_int(...)`)
        args = split_args(toks, i + 1)
        if strict and callee in NOT_READS:
            for a in args:
                used.update(x.idx for x in a if x.text in names)
            continue
        if callee not in GETTERS:
            continue
        pi, ki, di = GETTERS[callee]
        via = None
        if pi is None:
            if IMPLICIT_TU.get(callee) != tu_src:
                continue
        else:
            if len(args) <= pi or len(args[pi]) != 1 or args[pi][0].kind != 'id':
                continue
            nm = args[pi][0].text
            if nm in names:
                used.add(args[pi][0].idx)
            elif strict and alias_at(nm, i):
                via = nm
            else:
                continue
        fn = _fn_at(spans, i)
        if len(args) <= ki:
            errs.append('%s: %s() has no key argument' % (site_of(tu_src, t), callee))
            continue
        karg = args[ki]
        lits = [x for x in karg if x.kind == 'str']
        default = '-'
        if di is not None:
            if len(args) <= di:
                errs.append('%s: %s() has no default argument' % (site_of(tu_src, t), callee))
                continue
            dt = args[di]
            why = side_effects(dt, names)
            if why:
                errs.append('(a) %s: %s() default `%s` has a side effect (%s)'
                            % (site_of(tu_src, t), callee, ' '.join(x.text for x in dt), why))
            default = canon(toks, dt, names)
        if via:
            keys = []
            for x in lits:
                k = x.text[1:-1]
                if k not in ALIAS_KEYS:
                    errs.append('%s: %s(%s, "%s") reads through an alias of the harness path; '
                                'classify the key in ALIAS_KEYS' % (site_of(tu_src, t), callee, via, k))
                elif ALIAS_KEYS[k]:
                    keys.append(k)
            if not lits:
                errs.append('%s: %s(%s, ...) through an alias with no literal key' % (site_of(tu_src, t), callee, via))
            for k in keys:
                rows.append((k, '%s[%s]' % (callee, via), default, site_of(tu_src, t)))
            continue
        if len(karg) == 1 and karg[0].kind == 'str':
            rows.append((karg[0].text[1:-1], callee, default, site_of(tu_src, t)))
        elif (tu_src, fn) in DYNAMIC_OK:
            continue
        else:
            errs.append('%s: %s() in %s reads the harness with a non-literal key `%s` (not in DYNAMIC_OK)'
                        % (site_of(tu_src, t), callee, fn, ' '.join(x.text for x in karg)))
    # the folded reads of a player build (C2-3): one row each, nested ones included, as the
    # getter scan above finds a rig build's
    if strict:
        for j, t in enumerate(toks):
            f = fold_at(toks, j)
            if f is None:
                continue
            getter, macro, key, args, end = f
            default = '-'
            if getter != 'fe_ini_get':
                why = side_effects(args[0], names)
                if why:
                    errs.append('(a) %s: %s() default `%s` has a side effect (%s)'
                                % (site_of(tu_src, key), macro, ' '.join(x.text for x in args[0]), why))
                default = canon(toks, args[0], names)
            rows.append((key.text[1:-1], getter, default, site_of(tu_src, key)))
    # the key list lab_audit_keys re-reads (`static const char *const keys[] = {...}`)
    if tu_src.startswith('lab_'):
        found = {(r[0], r[3]) for r in rows}
        fa = C.functions(toks).get('lab_audit_keys')
        for i, t in enumerate(toks):
            if t.kind != 'str' or not LAB_KEY_RE.match(t.text):
                continue
            k, site = t.text[1:-1], site_of(tu_src, t)
            if fa and fa[0] <= i <= fa[1]:
                rows.append((k, 'fe_ini_get[lab_audit_keys]', '-', site))
            elif (k, site) in found or any(r[0] == k for r in rows):
                continue
            elif k not in LAB_NOT_KEYS:
                errs.append('%s: literal "%s" in a lab file is neither a read key nor in LAB_NOT_KEYS' % (site, k))
    if strict:
        for i, t in enumerate(toks):
            if t.kind == 'id' and t.text in names and i not in used:
                prev = toks[i - 1].text if i else ''
                if prev in ('char',) or (i + 1 < len(toks) and toks[i + 1].text == '[' and prev in ('char', ',')):
                    continue                             # its declaration
                errs.append('%s: unclassified use of %s (a read the table would miss? classify it in '
                            'GETTERS / NOT_READS / ALIAS_KEYS)' % (site_of(tu_src, t), t.text))
    return rows, errs


# ------------------------------------------------------------- table
def merge(per_flavour):
    """per_flavour: {flavour: [(key, getter, default, site)]} -> rows, errors (check b)."""
    sites, errs = {}, []
    for fl in FLAVOURS:
        for key, getter, default, site in per_flavour.get(fl, ()):
            r = sites.setdefault((key, site, getter), {})
            r[fl] = default
    rows = []
    for (key, site, getter), d in sites.items():
        vals = sorted(set(d.values()))
        if len(vals) > 1:
            errs.append('(b) %s "%s": the default differs between flavours: %s'
                        % (site, key, '; '.join('%s=`%s`' % (f, d[f]) for f in FLAVOURS if f in d)))
        fl = ','.join(f for f in FLAVOURS if f in d)
        rows.append((key, getter, d[[f for f in FLAVOURS if f in d][0]], site, fl))
    rows.sort(key=lambda r: (r[0], _site_key(r[3]), r[1]))
    return rows, errs


def _site_key(site):
    f, _, n = site.rpartition(':')
    return (f, int(n))


HEADER = ('# harness-ini keys: every read of the harness ini, per site and flavour.\n'
          '# GENERATED by tools/refactor/harness_keys.py gen from psp-gcc -E of release, lab, harness\n'
          '# (flavours.sh).  Do not edit; the GBAdhoc Lab reads this file (backend/lab/inikeys.py).\n'
          '# check: tools/refactor/harness_keys.py check (host suite harness_keys).\n')
COLS = 'key\tgetter\tdefault\tsite\tflavours'


def write_tsv(rows, path):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(HEADER)
        f.write(COLS + '\n')
        for r in rows:
            for x in r:
                if '\t' in x or '\n' in x:
                    raise Fail('a field holds a tab/newline: %r' % (r,))
            f.write('\t'.join(r) + '\n')


def read_tsv(path):
    rows = []
    with open(path, encoding='utf-8') as f:
        for ln in f:
            ln = ln.rstrip('\n')
            if not ln or ln.startswith('#'):
                continue
            if ln == COLS:
                continue
            p = ln.split('\t')
            if len(p) != 5:
                raise Fail('%s: malformed row %r' % (path, ln))
            rows.append(tuple(p))
    return rows


# ------------------------------------------------------------- gen
def pp_views(o):
    """{flavour: [(tu_src, names, tokens)]}."""
    out = {}
    for fl in FLAVOURS:
        views = []
        for obj, src_name, names in TUS:
            if o.from_pp:
                p = os.path.join(o.from_pp, fl, obj[:-2] + '.i')
                if not os.path.exists(p):
                    continue
            else:
                import aec_pspcc as P
                fdir = os.path.join(o.run, fl)
                try:
                    P.compile_argv(fdir, obj)
                except SystemExit:
                    continue                     # this flavour does not build the object
                text = P.preprocess(o.src, fdir, obj=obj)
                d = o.pp_dir or tempfile.mkdtemp(prefix='harness_keys_')
                os.makedirs(os.path.join(d, fl), exist_ok=True)
                p = os.path.join(d, fl, obj[:-2] + '.i')
                with open(p, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(text)
            views.append((src_name, names, C.load_pp(p, src_name)))
        out[fl] = views
    return out


def gen(o):
    per, errs = {}, []
    for fl, views in pp_views(o).items():
        if not any(s == 'main_psp.c' for s, _, _ in views):
            raise Fail('no main_psp.o -E for flavour %s' % fl)
        rows = []
        for src_name, names, toks in views:
            r, e = extract(toks, src_name, names)
            rows += r
            errs += ['[%s] %s' % (fl, x) for x in e]
        per[fl] = rows
    rows, e = merge(per)
    errs += e
    opt, e = opt_in_rows(o.src, {r[0] for r in rows})
    rows = sorted(rows + opt, key=lambda r: (r[0], _site_key(r[3]), r[1]))
    errs += e
    errs = sorted(set(errs))
    for x in errs:
        print('FAIL ' + x)
    if errs:
        return 1
    write_tsv(rows, o.out)
    print('harness_keys: %d sites, %d keys -> %s' % (len(rows), len({r[0] for r in rows}), o.out))
    for fl in FLAVOURS:
        print('  %-8s %d sites, %d keys' % (fl, sum(1 for r in rows if fl in r[4].split(',')),
                                            len({r[0] for r in rows if fl in r[4].split(',')})))
    return 0


# ------------------------------------------------------------- opt-in arms (raw source)
def _ifs_label(ifs):
    c = ifs[-1] if ifs else '?'
    c = re.sub(r'^#\s*(ifdef|ifndef|if)\s+', '', c.split(' | ')[0]).strip()
    return 'opt-in:' + re.sub(r'\s+', '', c)


def opt_in_rows(src, have):
    """rows for keys no generated flavour compiles: a HARNESS_INI read in an #if arm that only an
    opt-in switch turns on (FE_EXTRA=-DJIT_COST_KEYS, DRPROF_HW, ...; tools/build.sh:73).  A Lab
    job may build that arm, so its key stays known; the flavours column names the arm."""
    rows, errs = [], []
    root = os.path.join(src, 'psp')
    for dp, dns, fns in os.walk(root):
        dns[:] = sorted(d for d in dns if d not in ('tests',))
        for fn in sorted(fns):
            if not fn.endswith(('.c', '.h')):
                continue
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, root).replace('\\', '/')
            try:
                toks = C.load_raw(p)
            except ValueError:
                continue
            for i, t in enumerate(toks):
                if (t.kind != 'id' or i + 1 >= len(toks) or toks[i + 1].text != '(' or toks[i + 1].pair is None):
                    continue
                if t.text in MACRO_GETTERS:
                    getter, ki, di = MACRO_GETTERS[t.text]
                    args = split_args(toks, i + 1)
                    if len(args) <= ki:
                        continue
                elif t.text in GETTERS and GETTERS[t.text][0] == 0:
                    getter = t.text
                    pi, ki, di = GETTERS[t.text]
                    args = split_args(toks, i + 1)
                    if len(args) <= ki or len(args[pi]) != 1 or args[pi][0].text != 'HARNESS_INI':
                        continue
                else:
                    continue
                if len(args[ki]) != 1 or args[ki][0].kind != 'str' or args[ki][0].text[1:-1] in have:
                    continue
                key, site = args[ki][0].text[1:-1], 'psp/%s:%d' % (rel, int(t.line))
                default = '-'
                if di is not None and len(args) > di:
                    default = ' '.join(x.text for x in args[di])
                    why = side_effects(args[di], ('HARNESS_INI',))
                    if why:
                        errs.append('(a) %s: %s() default `%s` has a side effect (%s)' % (site, t.text, default, why))
                rows.append((key, getter, default, site, _ifs_label(t.ifs)))
    return rows, errs


# ------------------------------------------------------------- check (no docker)
_CMT = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
RAW_CALL = re.compile(r'(?:fe_ini_get(?:_int)?\s*\(\s*(?:HARNESS_INI|p_harness)\s*,'
                      r'|\bHARNESS_(?:INT|STR|FPS_X100)\s*\()\s*"([a-z0-9_]+)"')
RAW_RW = re.compile(r'\bRW_(?:KEY|FLAG)\s*\(\s*\w+\s*,\s*"([a-z0-9_]+)"')


def _strip_comments(text):
    # keep line numbers; string literals holding /* are not expected in these reads
    return _CMT.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text)


def raw_keys(src):
    """{key: [psp/FILE:LINE]} of the raw greps over psp/** (comments stripped, every #if arm)."""
    res = {}
    root = os.path.join(src, 'psp')
    for dp, dns, fns in os.walk(root):
        dns[:] = sorted(d for d in dns if d not in ('tests',))
        for fn in sorted(fns):
            if not fn.endswith(('.c', '.h')):
                continue
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, src).replace('\\', '/')
            text = _strip_comments(open(p, encoding='utf-8', errors='replace').read())
            for rx in (RAW_CALL, RAW_RW):
                for m in rx.finditer(text):
                    ln = text.count('\n', 0, m.start(1)) + 1
                    res.setdefault(m.group(1), []).append('%s:%d' % (rel, ln))
    return res


def check(o):
    rows = read_tsv(o.tsv)
    fails = []
    keys = {r[0] for r in rows}
    cache = {}
    for key, getter, default, site, fl in rows:
        if default != '-':
            why = side_effects(_lex(default))
            if why:
                fails.append('(a) %s "%s": default `%s` has a side effect (%s)' % (site, key, default, why))
        f, _, n = site.rpartition(':')
        p = os.path.join(o.src, f)
        if p not in cache:
            cache[p] = open(p, encoding='utf-8', errors='replace').read().split('\n') if os.path.exists(p) else None
        lines = cache[p]
        if lines is None or int(n) > len(lines) or                 not any('"%s"' % key in x for x in lines[int(n) - 1:int(n) + 2]):
            fails.append('stale: %s no longer holds "%s" (regenerate: harness_keys.py gen)' % (site, key))
        if not fl or not (fl.startswith('opt-in:') or all(x in FLAVOURS for x in fl.split(','))):
            fails.append('%s "%s": bad flavours column %r' % (site, key, fl))
    raw = raw_keys(o.src)
    missing = sorted(set(raw) - keys)
    for k in missing:
        fails.append('raw grep key "%s" (%s) is in no table row: no generated flavour compiles it, or the '
                     'table is stale' % (k, ', '.join(raw[k][:3])))
    extra = sorted(keys - set(raw))
    print('harness_keys check: %d sites, %d keys in %s' % (len(rows), len(keys), os.path.relpath(o.tsv, o.src)))
    print('  raw greps (HARNESS_INT/HARNESS_STR("k") + fe_ini_get*(HARNESS_INI, "k") + RW_KEY/RW_FLAG, psp/**): '
          '%d keys, all in the table: %s'
          % (len(raw), 'yes' if not missing else 'NO (%d missing)' % len(missing)))
    print('  table keys the raw greps do not see (helpers, aliases, online/lab readers): %d: %s'
          % (len(extra), ' '.join(extra)))
    for fl in FLAVOURS:
        print('  %-8s %d keys' % (fl, len({r[0] for r in rows if fl in r[4].split(',')})))
    if o.expect_keys is not None and len(keys) != o.expect_keys:
        fails.append('key count %d != pinned %d (a key was added or removed: re-pin it in the host test)'
                     % (len(keys), o.expect_keys))
    if o.expect_sites is not None and len(rows) != o.expect_sites:
        fails.append('site count %d != pinned %d' % (len(rows), o.expect_sites))
    for x in fails:
        print('FAIL ' + x)
    print('harness_keys check: %s' % ('PASS' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


def _lex(text, line=1, file=None):
    out = []
    C._lex_line(text, line, (), out, file)
    return out


# ------------------------------------------------------------- selftest
def _tu(text, fname='main_psp.c'):
    d = tempfile.mkdtemp(prefix='hk_selftest_')
    p = os.path.join(d, 'x.i')
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# 1 "%s"\n' % fname)
        f.write(text)
    return C.load_pp(p, fname)


def selftest():
    fails = []

    def ok(cond, what):
        print(('ok   ' if cond else 'FAIL ') + what)
        if not cond:
            fails.append(what)

    base = ('static char p_harness[160], p_config[160];\n'
            'void boot(void) {\n'
            '  long a = fe_ini_get_int(p_harness, "alpha", 0);\n'
            '  fe_ini_get(p_harness, "beta", buf, sizeof(buf));\n'
            '  g.x = (uint32_t)fe_ini_get_int(p_harness, "rewind_mib", (long)g.x);\n'
            '  fe_ini_never(p_harness); have = file_exists(p_harness);\n'
            '  long c = fe_ini_get_int(p_config, "cfgkey", 0);\n'
            '  { const char *src = hv ? p_variant : p_harness;\n'
            '    long s = fe_ini_get_int(src, hv ? "silent_wireless" : "silent", 0); }\n'
            '}\n')
    r, e = extract(_tu(base), 'main_psp.c', ('p_harness',))
    keys = sorted(x[0] for x in r)
    ok(not e and keys == ['alpha', 'beta', 'rewind_mib', 'silent'], 'base: alpha/beta/rewind_mib/silent read, '
       'config and variant spelling ignored (%s %s)' % (keys, e))
    ok(any(x[0] == 'rewind_mib' and x[2] == '( long ) g . x' for x in r), 'default text of a macro read')
    ok(any(x[0] == 'silent' and x[1] == 'fe_ini_get_int[src]' for x in r), 'alias read tagged')
    for name, mut in (('call', '  long m = fe_ini_get_int(p_harness, "m", next_val());\n'),
                      ('assign', '  long m = fe_ini_get_int(p_harness, "m", g = 2);\n'),
                      ('incr', '  long m = fe_ini_get_int(p_harness, "m", n++);\n')):
        r2, e2 = extract(_tu(base.replace('}\n', mut + '}\n', 1)[::1]), 'main_psp.c', ('p_harness',))
        ok(any(x.startswith('(a)') for x in e2), 'M-%s: a default with a side effect fails (a)' % name)
    ok(side_effects(_lex('( long ) sizeof ( x ) + PLAY_A == 2')) is None, 'a cast/sizeof/== default passes (a)')
    ok(side_effects(_lex('! hv && fe_ini_get_int ( p_harness , "browser" , 0 ) != 0')) is None,
       'a nested harness read in a default passes (a)')
    ok(side_effects(_lex('fe_ini_get_int ( p_config , "browser" , 0 )')) is not None,
       'a nested read of another ini fails (a)')
    r3, e3 = extract(_tu(base.replace('have = file_exists(p_harness);', 'have = my_reader(p_harness, "z");')),
                     'main_psp.c', ('p_harness',))
    ok(any('unclassified use of p_harness' in x for x in e3), 'M-unclassified: an unknown reader of p_harness fails')
    r4, e4 = extract(_tu(base.replace('"silent", 0', '"quiet", 0')), 'main_psp.c', ('p_harness',))
    ok(any('ALIAS_KEYS' in x for x in e4), 'M-alias: an unlisted key through an alias fails')
    r5, e5 = extract(_tu(base.replace('"alpha"', 'kname')), 'main_psp.c', ('p_harness',))
    ok(any('non-literal key' in x for x in e5), 'M-dynamic: a non-literal key fails outside DYNAMIC_OK')
    rows, eb = merge({'release': [('alpha', 'fe_ini_get_int', '0', 'psp/a.c:3')],
                      'harness': [('alpha', 'fe_ini_get_int', '1', 'psp/a.c:3')]})
    ok(any(x.startswith('(b)') for x in eb), 'M-drift: a default that differs between flavours fails (b)')
    rows, eb = merge({'release': [('alpha', 'fe_ini_get_int', '0', 'psp/a.c:3')],
                      'lab': [('alpha', 'fe_ini_get_int', '0', 'psp/a.c:3')],
                      'harness': [('alpha', 'fe_ini_get_int', '0', 'psp/a.c:3'), ('omega', 'fe_ini_get', '-', 'psp/a.c:9')]})
    ok(not eb and rows == [('alpha', 'fe_ini_get_int', '0', 'psp/a.c:3', 'release,lab,harness'),
                           ('omega', 'fe_ini_get', '-', 'psp/a.c:9', 'harness')], 'merge: flavours column')
    lab = ('static char L_ini[192];\n'
           'static int task_ini_int(const char *key, int def) { return (int)fe_ini_get_int(L_ini, key, def); }\n'
           'void lab_audit_keys(void) { static const char *const keys[] = { "lab_v", "lab_task" };\n'
           '  unsigned i; for (i = 0; i < 2; i++) fe_ini_get(L_ini, keys[i], tmp, sizeof(tmp)); }\n'
           'void lab_go(void) { fe_ini_get_int(L_ini, "lab_v", 0); task_ini_int("lab_blink", 0);\n'
           '  finish("lab_wall_limit"); }\n')
    r6, e6 = extract(_tu(lab, 'lab_harness.c'), 'lab_harness.c', ('L_ini', 'ini'))
    ok(not e6 and sorted({x[0] for x in r6}) == ['lab_blink', 'lab_task', 'lab_v'],
       'lab: task_ini_int, L_ini reads, the audit list; lab_wall_limit is no key (%s %s)' % (sorted({x[0] for x in r6}), e6))
    r7, e7 = extract(_tu(lab.replace('"lab_wall_limit"', '"lab_new_thing"'), 'lab_harness.c'), 'lab_harness.c',
                     ('L_ini', 'ini'))
    ok(any('LAB_NOT_KEYS' in x for x in e7), 'M-lablit: an unknown standalone lab_* literal fails')
    on = ('static long get_int(const char *h, const char *c, const char *k, long def)\n'
          '{ if (fe_ini_get(h, k, b, sizeof(b))) return fe_ini_get_int(h, k, def); return def; }\n'
          'int online_psp_configure(const char *h, const char *c)\n'
          '{ x = (int)fe_ini_get_int(h, "online_prx_cycles", 0); y = get_int(h, c, "online_direct", 1); }\n')
    r8, e8 = extract(_tu(on, 'online_psp.c'), 'online_psp.c', ('h',))
    ok(not e8 and sorted(x[0] for x in r8) == ['online_direct', 'online_prx_cycles'] and
       r8[0][3].startswith('psp/online_psp.c:'), 'online: h reads and the get_int helper (%s %s)' % (r8, e8))
    # the folded form of a player build (C2-3): the same rows, defaults in fe_priv.h's spelling
    rig = ('static char p_harness[160];\nvoid boot(void) {\n'
           '  long a = fe_ini_get_int(p_harness, "alpha", 0);\n'
           '  fe_ini_get(p_harness, "beta", buf, sizeof(buf));\n'
           '  int f = pcfg_fps_x100(p_harness, "fps", g.f);\n'
           '  long n = fe_ini_get_int(p_harness, "outer", !hv && fe_ini_get_int(p_harness, "inner", 0) != 0);\n}\n')
    fold = ('static char p_config[160];\nvoid boot(void) {\n'
            '  long a = ((void)sizeof("alpha"), (long)(0));\n'
            '  ((void)sizeof("beta"), (void)(buf), (void)(sizeof(buf)), harness_not_found());\n'
            '  int f = ((void)sizeof("fps"), (int)(g.f));\n'
            '  long n = ((void)sizeof("outer"), (long)(!hv && ((void)sizeof("inner"), (long)(0)) != 0));\n}\n')
    rr, er = extract(_tu(rig), 'main_psp.c', ('p_harness',))
    rf, ef = extract(_tu(fold), 'main_psp.c', ('p_harness',))
    rows, eb = merge({'release': rf, 'harness': rr})
    ok(not er and not ef and not eb and sorted(rr) == sorted(rf) and len(rf) == 5 and
       ('outer', 'fe_ini_get_int', '! hv && HARNESS_INT ( "inner" , 0 ) != 0', 'psp/main_psp.c:6') in rf,
       'fold: the folded reads give the rig rows, nested default canonical (%s %s %s)' % (rf, ef, eb))
    rf2, _ = extract(_tu(fold.replace('(long)(0));\n  ((void)', '(long)(1));\n  ((void)', 1)), 'main_psp.c', ('p_harness',))
    rows, eb = merge({'release': rf2, 'harness': rr})
    ok(any(x.startswith('(b)') for x in eb), 'M-folddrift: a folded default that differs from the rig fails (b)')
    # opt-in arms in fe_priv.h's call form (C2-1): HARNESS_INT/HARNESS_STR in the raw source
    d = tempfile.mkdtemp(prefix='hk_optin_')
    os.makedirs(os.path.join(d, 'psp', 'fe'))
    with open(os.path.join(d, 'psp', 'fe', 'x.inc.c'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('#ifdef OPT_ARM\nvoid f(void) { char b[8];\n'
                '  long a = HARNESS_INT("opt_int", 3);\n'
                '  HARNESS_STR("opt_str", b, sizeof(b));\n'
                '  long k = HARNESS_INT("known", 0);\n'
                '  long n = HARNESS_INT("opt_nest", HARNESS_INT("known", 1));\n'
                '  long m = HARNESS_INT("opt_bad", next_val()); }\n#endif\n')
    try:
        r9, e9 = opt_in_rows(d, {'known'})
    finally:
        import shutil
        shutil.rmtree(d, ignore_errors=True)
    ok(sorted(r9) == sorted([('opt_int', 'fe_ini_get_int', '3', 'psp/fe/x.inc.c:3', 'opt-in:OPT_ARM'),
                             ('opt_str', 'fe_ini_get', '-', 'psp/fe/x.inc.c:4', 'opt-in:OPT_ARM'),
                             ('opt_nest', 'fe_ini_get_int', 'HARNESS_INT ( "known" , 1 )',
                              'psp/fe/x.inc.c:6', 'opt-in:OPT_ARM'),
                             ('opt_bad', 'fe_ini_get_int', 'next_val ( )', 'psp/fe/x.inc.c:7', 'opt-in:OPT_ARM')])
       and len(e9) == 1 and 'opt_bad' not in e9[0] and 'next_val' in e9[0],
       'opt-in: HARNESS_INT/HARNESS_STR rows, a nested read allowed, a call refused (%s %s)' % (r9, e9))
    print('selftest: %s' % ('PASS' if not fails else 'FAILED (%d)' % len(fails)))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    g = sub.add_parser('gen')
    g.add_argument('--run')
    g.add_argument('--from-pp')
    g.add_argument('--pp-dir')
    g.add_argument('--src', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    g.add_argument('--out', default=DEFAULT_TSV)
    c = sub.add_parser('check')
    c.add_argument('--src', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    c.add_argument('--tsv', default=DEFAULT_TSV)
    c.add_argument('--expect-keys', type=int)
    c.add_argument('--expect-sites', type=int)
    sub.add_parser('selftest')
    o = ap.parse_args()
    try:
        if o.cmd == 'gen':
            if bool(o.run) == bool(o.from_pp):
                ap.error('gen: give exactly one of --run / --from-pp')
            return gen(o)
        if o.cmd == 'check':
            return check(o)
        if o.cmd == 'selftest':
            return selftest()
    except Fail as e:
        print('FAIL %s' % e)
        return 1
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
