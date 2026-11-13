#!/usr/bin/env python3
"""ci_greps.py -- the rule engine behind T11 ci_greps.sh (PLAN §5).  Source rules for the
main_psp.c split, run over psp/ and frontend-common/ (comments and strings are not code:
every rule works on aec_cparse tokens, not on raw grep lines).

  R1  exits: no raw `sceKernelExitGame(` / `handoff_run(` call outside fe_priv.h.
      --phase base   today's contract: in main_psp.c every call sits after the hijack
                     macros (L:2596-2597), so each one is wrapped; with --pp DIR the -E
                     output of each flavour is checked too: every sceKernelExitGame and
                     handoff_run call in main_psp.c is preceded by `pwr_release_all(),`.
      --phase P      the P3 contract: zero raw calls outside fe_priv.h (no exception).
      --phase HW2    as P, plus the allow-file entries marked phase=HW2 (stall_exit).
      Files that ARE the handoff/exit implementation are allow-listed with a reason.
  R2  no function declared `cold` / `noreturn` (attribute, _Noreturn, or a macro whose
      body says so) without an explicit `section(...)` in the same declaration
      (PLAN §0.2-5, §4.3-2: either would land in .text.unlikely ahead of main).
  R3  no pwr_hold reachable from a gpsp_* hook (they run nested in fe_host_run_frame,
      r4): direct calls and calls through same-file functions, with the path.
  R4  no logic inside fe_evt(...) arguments (they vanish in release, r6 B3):
      assignments, ++/--, calls to mutating functions (name pattern), and any call whose
      name is not in the reviewed list ci_greps.fe_evt_calls (pure getters seen at base).
  R5  every .c that tests GPSP_NO_TELEMETRY includes fe_evt.h (directly or through a
      tree header) BEFORE the first test (fe_evt.h:44-46 derives it).
  R6  (Epoch C, PLAN-EPOCHC-beta5 §4.2) every static function of main_psp.c's unit that
      ONLY main() calls carries `noinline` AND an explicit `section(...)`, unless it is
      one of the functions pinned in ci_greps.r6_pinned (the ones the start tree already
      has: their placement is part of today's layout).  Why: GCC puts a function that is
      called only at startup into `.text.startup.<name>` on its own and emits it BEFORE
      main in main_psp.o (C0a-6 probe), and it inlines a static with one caller back into
      main; either moves the frame loop with every later edit to that function.
      --write-r6 writes the pinned list from the tree (do it once, on the tree Epoch C
      starts from).
Known exceptions live in ci_greps.known (rule, file, key, reason); they are reported as
KNOWN and do not fail.  Exit: 0 green, 1 violations, 2 usage.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import aec_cparse as C  # noqa: E402

SCOPE = ('psp', 'frontend-common')
SKIP_DIRS = ('launcher', 'tests', 'labab')
# a call that DOES something: an SDK verb (sceCtrlSetSamplingCycle, sceKernelDelete...) or a
# snake_case name whose first or last word is a verb (audio_stop, set_x); getters such as
# fe_np_start_reason or vram_free_base end in a noun and pass.
_VERBS = ('set|start|stop|init|apply|write|free|alloc|open|close|reset|push|pop|send|flush|'
          'enqueue|clear|hold|release|take|drop|kick|fire|load|save|delete|create|signal|wait|'
          'lock|unlock|resume|suspend|teardown|shutdown|kill|destroy|inject')
MUTATOR = re.compile(r'^sce[A-Z][A-Za-z]*?(Set|Start|Stop|Create|Delete|Write|Change|Lock|Unlock|'
                     r'Signal|Wait|Sleep|Delay|Exit|Load|Unload)(?=[A-Z]|$)'
                     r'|^(%s)(_|$)|_(%s)$|(^|_)(kill|destroy|inject|corrupt|trash)(_|$)'
                     % (_VERBS, _VERBS))


def files(src, exts=('.c', '.h', '.inc', '.inc.c')):
    out = []
    for d in SCOPE:
        for root, dirs, fns in os.walk(os.path.join(src, d)):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
            for fn in fns:
                if fn.endswith(exts):
                    out.append(os.path.relpath(os.path.join(root, fn), src).replace('\\', '/'))
    return sorted(out)


def load_known(path):
    k = []
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            p = line.rstrip('\n').split('\t')
            k.append(dict(rule=p[0], file=p[1], key=p[2], reason=p[3] if len(p) > 3 else '',
                          phase=p[4] if len(p) > 4 else ''))
    return k


class Hit(object):
    def __init__(self, rule, file, line, key, msg):
        self.rule, self.file, self.line, self.key, self.msg = rule, file, line, key, msg


def toks_of(src, rel, cache={}):
    p = os.path.join(src, rel)
    if p not in cache:
        try:
            cache[p] = C.load_raw(p)
        except ValueError:            # unbalanced across #if branches: lex only
            cache[p] = None
    return cache[p]


def raw_lex(src, rel):
    """tokens without bracket linking (for files whose #if branches unbalance braces)."""
    t = toks_of(src, rel)
    if t is not None:
        return t
    out = []
    in_c = False
    for i, line in enumerate(open(os.path.join(src, rel), encoding='utf-8', errors='replace'), 1):
        s = line
        if in_c:
            e = s.find('*/')
            if e < 0:
                continue
            s = s[e + 2:]
            in_c = False
        if s.lstrip().startswith('#'):
            continue
        s = re.sub(r'/\*.*?\*/', ' ', s)
        if '/*' in s:
            s, in_c = s[:s.index('/*')], True
        s = re.sub(r'//.*', '', s)
        C._lex_line(s, i, (), out)
    return out


# ------------------------------------------------------------- R1
def rule_r1(src, phase, known, pp_dir):
    hits, info = [], []
    allow_files = set(k['file'] for k in known if k['rule'] == 'R1' and k['key'] == '*')
    for rel in files(src, ('.c', '.inc', '.inc.c', '.h')):
        if rel.endswith('fe_priv.h') or rel in allow_files:
            continue
        t = raw_lex(src, rel)
        hijack = {}
        if rel == 'psp/main_psp.c':
            for i, line in enumerate(open(os.path.join(src, rel), encoding='utf-8', errors='replace'), 1):
                m = re.match(r'\s*#\s*define\s+(sceKernelExitGame|handoff_run)\s*\(', line)
                if m:
                    hijack[m.group(1)] = i
        fn_at = _function_spans(t)
        for k in range(len(t) - 1):
            a = t[k]
            if a.text in ('sceKernelExitGame', 'handoff_run') and t[k + 1].text == '(' and \
                    not (k > 0 and t[k - 1].kind == 'id' and t[k - 1].text in ('void', 'int')):
                fn = _fn_at(fn_at, k)
                key = '%s@%s' % (a.text, fn or '-')
                if phase == 'base' and a.text in hijack and a.line > hijack[a.text]:
                    info.append((rel, a.line, key))
                    continue
                if phase == 'HW2' and any(x['rule'] == 'R1' and x['file'] == rel and x['key'] == key
                                          and x['phase'] == 'HW2' for x in known):
                    info.append((rel, a.line, key + ' (HW2 whitelist)'))
                    continue
                hits.append(Hit('R1', rel, a.line, key, 'raw %s( outside fe_priv.h' % a.text))
            elif a.text in ('sceKernelExitGame', 'handoff_run') and t[k + 1].text != '(':
                # address taken (`q = sceKernelExitGame; q();`): an unwrapped exit
                fn = _fn_at(fn_at, k)
                hits.append(Hit('R1', rel, a.line, '%s&@%s' % (a.text, fn or '-'),
                                '%s referenced without a call (address taken)' % a.text))
        # macro bodies: the token pass drops preprocessor lines, so a raw exit inside a
        # `#define` of any other file was invisible (verifier 2026-11-05)
        lines = open(os.path.join(src, rel), encoding='utf-8', errors='replace').read().split('\n')
        i = 0
        while i < len(lines):
            line, first = lines[i], i + 1
            while line.rstrip().endswith('\\') and i + 1 < len(lines):
                i += 1
                line = line.rstrip()[:-1] + ' ' + lines[i]
            i += 1
            m = re.match(r'\s*#\s*define\s+(\w+)(\([^)]*\))?(.*)', line)
            if not m:
                continue
            body = re.sub(r'/\*.*?\*/|//.*', ' ', m.group(3))
            for nm in ('sceKernelExitGame', 'handoff_run'):
                if not re.search(r'\b%s\b' % nm, body):
                    continue
                if phase == 'base' and rel == 'psp/main_psp.c' and m.group(1) == nm:
                    info.append((rel, first, '%s@#define %s (the hijack)' % (nm, m.group(1))))
                    continue
                hits.append(Hit('R1', rel, first, '%s@#define %s' % (nm, m.group(1)),
                                'raw %s in the body of macro %s outside fe_priv.h' % (nm, m.group(1))))
    extra = []
    if pp_dir:
        for fl in ('release', 'lab', 'harness'):
            p = os.path.join(pp_dir, 'main_psp.%s.i' % fl)
            if not os.path.exists(p):
                continue
            t = C.load_pp(p)
            n = w = 0
            for k in range(len(t) - 1):
                if t[k].text in ('sceKernelExitGame', 'handoff_run') and t[k + 1].text == '(':
                    if k > 0 and t[k - 1].text in ('void', 'int'):
                        continue                      # a declaration
                    n += 1
                    if _wrapped(t, k):
                        w += 1
                    else:
                        hits.append(Hit('R1e', 'psp/main_psp.c[%s -E]' % fl, t[k].line, t[k].text,
                                        'call not wrapped by pwr_release_all()'))
            extra.append('  R1e %-8s -E: %d exit/handoff calls in main_psp.c, %d wrapped' % (fl, n, w))
    return hits, info, extra


def _wrapped(t, k, levels=3):
    """True when the call at t[k] sits inside a parenthesised group, at most `levels`
    groups out, whose first tokens are `pwr_release_all ( ) ,` -- the hijack's shape in
    release `(pwr_release_all(), handoff_run(..))` and in lab/harness
    `(pwr_release_all(), (lab_active() ? lab_finish(..) : handoff_run(..)))`.
    (Verifier 2026-11-05: the old token-window test accepted any call after `:` and
    skipped any call after `?`, so a raw `c ? 0 : sceKernelExitGame()` passed.)"""
    j, depth = k - 1, 0
    while j >= 0 and levels > 0:
        x = t[j].text
        if x == ')':
            depth += 1
        elif x == '(':
            if depth == 0:
                if [y.text for y in t[j + 1:j + 5]] == ['pwr_release_all', '(', ')', ',']:
                    return True
                levels -= 1
            else:
                depth -= 1
        elif x in (';', '{', '}') and depth == 0:
            return False
        j -= 1
    return False


def _function_spans(t):
    try:
        f = C.functions(t)
    except Exception:
        return []
    return sorted((lo, hi, name) for name, (lo, hi) in f.items())


def _fn_at(spans, k):
    for lo, hi, name in spans:
        if lo <= k <= hi:
            return name
    return None


# ------------------------------------------------------------- R2
def rule_r2(src):
    hits, n = [], 0
    macros = {}
    for rel in files(src):
        for line in open(os.path.join(src, rel), encoding='utf-8', errors='replace'):
            m = re.match(r'\s*#\s*define\s+(\w+)\s+(.*)', line)
            if m and re.search(r'\bcold\b|noreturn', m.group(2)) and '__attribute__' in m.group(2):
                macros[m.group(1)] = 'section' in m.group(2)
    for rel in files(src):
        t = raw_lex(src, rel)
        k = 0
        while k < len(t):
            x = t[k].text
            bad = None
            if x == '__attribute__' and k + 2 < len(t):
                j = k + 1
                depth = 0
                inner = []
                while j < len(t):
                    if t[j].text == '(':
                        depth += 1
                    elif t[j].text == ')':
                        depth -= 1
                        if depth == 0:
                            break
                    inner.append(t[j].text)
                    j += 1
                if 'cold' in inner or 'noreturn' in inner or '__cold__' in inner or '__noreturn__' in inner:
                    bad = 'attribute'
            elif x in ('_Noreturn', 'noreturn'):
                bad = x
            elif x in macros and not macros[x]:
                bad = 'macro ' + x
            if bad:
                n += 1
                # the declaration: back to the previous ; } { and forward to ; or {
                a = k
                while a > 0 and t[a - 1].text not in (';', '}', '{'):
                    a -= 1
                b = k
                while b < len(t) and t[b].text not in (';', '{'):
                    b += 1
                decl = [y.text for y in t[a:b]]
                if 'section' not in decl and '__section__' not in decl:
                    name = next((t[i].text for i in range(b - 1, a - 1, -1)
                                 if t[i].kind == 'id' and i + 1 < len(t) and t[i + 1].text == '('
                                 and t[i].text not in C.KEYWORDS), '?')
                    hits.append(Hit('R2', rel, t[k].line, name, '%s without section()' % bad))
            k += 1
    return hits, n


# ------------------------------------------------------------- R6
def r6_main_only(src):
    """[(name, line, has_noinline, has_section)] of the static functions of main_psp.c's
    unit whose only caller is main()."""
    rel = 'psp/main_psp.c'
    t = toks_of(src, rel)
    if t is None:
        return None
    fns = C.functions(t)
    if 'main' not in fns:
        return []
    callers = {}
    for name, (lo, hi) in fns.items():
        try:
            cl = [c.name for c in C.walk_calls(C.parse_body(t, lo, hi))]
        except Exception:
            cl = [x.text for i, x in enumerate(t[lo:hi]) if x.kind == 'id' and t[lo + i + 1].text == '(']
        for c in set(cl):
            callers.setdefault(c, set()).add(name)
    # a function whose name appears without a call (address taken: a thread entry, a
    # callback) can run from anywhere
    taken = set()
    for k, x in enumerate(t):
        if x.kind == 'id' and x.text in fns and k + 1 < len(t) and t[k + 1].text != '(' \
                and not (k > 0 and t[k - 1].text in ('.', '->')):
            taken.add(x.text)
    out = []
    for name, (lo, hi) in sorted(fns.items(), key=lambda kv: kv[1][0]):
        if name == 'main' or callers.get(name) != {'main'} or name in taken:
            continue
        a = lo
        while a > 0 and t[a - 1].text not in (';', '}'):
            a -= 1
        decl = [y.text for y in t[a:lo]]
        if 'static' not in decl:
            continue
        out.append((name, t[lo].line, 'noinline' in decl or '__noinline__' in decl,
                    'section' in decl or '__section__' in decl,
                    'always_inline' in decl or '__always_inline__' in decl))
    return out


def rule_r6(src, pinned):
    """always_inline is the other answer (Epoch C stage s, PLAN-EPOCHC-beta5 §3 C1): the
    body is pasted into main() by the compiler, as the copy it replaced was, and no
    function of its own is placed anywhere."""
    hits = []
    rows = r6_main_only(src) or []
    for name, line, ni, sec, ai in rows:
        if name in pinned or (ni and sec) or (ai and not ni):
            continue
        miss = ' and '.join(x for x, have in (('noinline', ni), ('section()', sec)) if not have)
        hits.append(Hit('R6', 'psp/main_psp.c', line, name,
                        'only main() calls it and it has no %s' % miss))
    return hits, len(rows)


def load_r6(path):
    if not path or not os.path.exists(path):
        return set()
    return {l.split()[0] for l in open(path, encoding='utf-8') if l.strip() and not l.startswith('#')}


def write_r6(src, path):
    view, _ = logical_view(src)
    try:
        rows = r6_main_only(view) or []
    finally:
        if view != src:
            import shutil
            shutil.rmtree(view, ignore_errors=True)
    names = sorted(n for n, _, ni, sec, ai in rows if not (ni and sec) and not (ai and not ni))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# T11 R6 pinned list: static functions of main_psp.c\'s unit that only main() calls\n'
                '# and that have no noinline + section() pair.  They exist on the tree this list was\n'
                '# written from, so where GCC puts them is part of that tree\'s layout.  A function\n'
                '# NOT listed here must carry both attributes (ci_greps.py R6).  Regenerate with\n'
                '# `ci_greps.py --src GPSP --write-r6` on the tree Epoch C starts from, never to\n'
                '# silence a new function.\n')
        for n in names:
            f.write(n + '\n')
    return len(names)


# ------------------------------------------------------------- R3
# Non-gpsp_* functions that ALSO run nested in fe_host_run_frame and are roots of
# R3: the core's ROM-load progress hook and the session-memory reload note run inside
# fe_host_gba_sessmem_cycle (PLAN-P-beta5 §2.1, P0a).
R3_EXTRA_ROOTS = ('gba_reload_note', 'gba_reload_progress')

def rule_r3(src):
    """The call graph is the UNION over every file in scope, not per file: after phase P a
    hook and its callees sit in different psp/fe/*.inc.c fragments of one TU, and a
    per-file graph would stop at the fragment edge (verifier 2026-11-05).  A name defined
    static in two TUs is merged (over-approximation: can only add paths, never hide one)."""
    hits, hooks = [], 0
    calls, per_file = {}, []
    for rel in files(src, ('.c', '.inc.c')):
        t = toks_of(src, rel)
        if t is None:
            continue
        fns = C.functions(t)
        for name, (lo, hi) in fns.items():
            try:
                cl = [c.name for c in C.walk_calls(C.parse_body(t, lo, hi))]
            except Exception:
                cl = [x.text for i, x in enumerate(t[lo:hi]) if x.kind == 'id'
                      and t[lo + i + 1].text == '(']
            calls.setdefault(name, []).extend(cl)
        per_file.append((rel, t, fns))
    # a function-like macro whose body calls pwr_hold is pwr_hold for this rule
    holds = {'pwr_hold'}
    for rel in files(src):
        for line in open(os.path.join(src, rel), encoding='utf-8', errors='replace'):
            m = re.match(r'\s*#\s*define\s+(\w+)\s*\((.*)', line)
            if m and re.search(r'\bpwr_hold\s*\(', m.group(2)):
                holds.add(m.group(1))
    for rel, t, fns in per_file:
        for name in sorted(fns):
            if not name.startswith('gpsp_') and name not in R3_EXTRA_ROOTS:
                continue
            hooks += 1
            seen, stack = {name}, [(name, [name])]
            while stack:
                f, path = stack.pop()
                for c in calls.get(f, []):
                    if c in holds:
                        hits.append(Hit('R3', rel, t[fns[name][0]].line, name,
                                        'pwr_hold reachable: %s' % ' -> '.join(
                                            path + ([c] if c == 'pwr_hold' else [c, 'pwr_hold']))))
                    elif c in calls and c not in seen:
                        seen.add(c)
                        stack.append((c, path + [c]))
    return hits, hooks


# ------------------------------------------------------------- R4
def fe_evt_args(src, with_fn=False):
    """yield (rel, line, arg tokens[, enclosing function]) for every fe_evt( call in
    scope (not its definition)."""
    for rel in files(src):
        if rel.endswith(('fe_evt.c', 'fe_evt.h')):
            continue
        t = raw_lex(src, rel)
        spans = _function_spans(t) if with_fn else None
        for k in range(len(t) - 1):
            if t[k].text == 'fe_evt' and t[k + 1].text == '(' and not (k > 0 and t[k - 1].text in ('void', '*')):
                d, j = 0, k + 1
                while j < len(t):
                    if t[j].text == '(':
                        d += 1
                    elif t[j].text == ')':
                        d -= 1
                        if d == 0:
                            break
                    j += 1
                if with_fn:
                    yield rel, t[k].line, t[k + 2:j], _fn_at(spans, k) or '-'
                else:
                    yield rel, t[k].line, t[k + 2:j]


def r4_key(fn, what, args):
    """KNOWN key of an R4 hit: the enclosing function, the offending token and the whole
    argument list's tokens (adversarial review 2026-11-07: keyed by call name and file,
    one KNOWN row excused every such call anywhere in main_psp.c, and none after the split)."""
    return '%s|%s|%s' % (fn, what, ' '.join(x.text for x in args))


def rule_r4(src, allow):
    hits, ncall, nsite = [], 0, 0
    # a macro that forwards to fe_evt hides its call sites' arguments from this rule
    # (none at 01067e74; verifier 2026-11-05)
    for rel in files(src):
        if rel.endswith(('fe_evt.c', 'fe_evt.h')):
            continue
        for i, line in enumerate(open(os.path.join(src, rel), encoding='utf-8', errors='replace'), 1):
            m = re.match(r'\s*#\s*define\s+(\w+)\b(.*)', line)
            if m and m.group(1) != 'fe_evt' and re.search(r'\bfe_evt\s*\(', m.group(2)):
                hits.append(Hit('R4', rel, i, '#define ' + m.group(1),
                                'macro %s forwards to fe_evt: its arguments escape R4' % m.group(1)))
    for rel, line, args, fn in fe_evt_args(src, with_fn=True):
        nsite += 1
        for i, a in enumerate(args):
            if a.text in ('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=', '++', '--'):
                hits.append(Hit('R4', rel, a.line, r4_key(fn, a.text, args), 'operator %s in fe_evt args' % a.text))
            if a.kind == 'id' and i + 1 < len(args) and args[i + 1].text == '(' and a.text not in C.KEYWORDS:
                ncall += 1
                if MUTATOR.search(a.text):
                    hits.append(Hit('R4', rel, a.line, r4_key(fn, a.text, args), 'mutating call %s() in fe_evt args' % a.text))
                elif allow is not None and a.text not in allow:
                    hits.append(Hit('R4', rel, a.line, r4_key(fn, a.text, args),
                                    'call %s() in fe_evt args is not in ci_greps.fe_evt_calls' % a.text))
    return hits, nsite, ncall


def write_allow(src, path):
    names = set()
    for rel, line, args in fe_evt_args(src):
        for i, a in enumerate(args):
            if a.kind == 'id' and i + 1 < len(args) and args[i + 1].text == '(' \
                    and a.text not in C.KEYWORDS and not MUTATOR.search(a.text):
                names.add(a.text)
    with open(path, 'w', newline='\n') as f:
        f.write('# T11 R4: functions reviewed as side-effect-free getters, allowed inside fe_evt(...)\n'
                '# arguments (generated from 01067e74, then reviewed; one name per line).\n')
        for n in sorted(names):
            f.write(n + '\n')
    return len(names)


def load_allow(path):
    if not path or not os.path.exists(path):
        return None
    return set(l.split('#')[0].strip() for l in open(path) if l.split('#')[0].strip())


# ------------------------------------------------------------- R5
def rule_r5(src):
    hits, n = [], 0
    provides = {}

    def gives(hdr_rel, depth=0):
        if hdr_rel in provides:
            return provides[hdr_rel]
        provides[hdr_rel] = False
        p = os.path.join(src, hdr_rel)
        if not os.path.exists(p) or depth > 8:
            return False
        for line in open(p, encoding='utf-8', errors='replace'):
            m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
            if m:
                inc = m.group(1)
                if os.path.basename(inc) == 'fe_evt.h':
                    provides[hdr_rel] = True
                    return True
                r = _resolve(src, hdr_rel, inc)
                if r and gives(r, depth + 1):
                    provides[hdr_rel] = True
                    return True
        return False
    for rel in files(src, ('.c', '.inc.c')):
        inc_ok = False
        for i, line in enumerate(open(os.path.join(src, rel), encoding='utf-8', errors='replace'), 1):
            m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
            if m:
                inc = m.group(1)
                if os.path.basename(inc) == 'fe_evt.h':
                    inc_ok = True
                else:
                    r = _resolve(src, rel, inc)
                    if r and gives(r):
                        inc_ok = True
            if re.match(r'\s*#\s*(if|ifdef|ifndef|elif)\b.*\bGPSP_NO_TELEMETRY\b', line):
                n += 1
                if not inc_ok:
                    hits.append(Hit('R5', rel, i, 'GPSP_NO_TELEMETRY',
                                    'tested before fe_evt.h is included'))
                break
    return hits, n


def _resolve(src, from_rel, inc):
    base = os.path.dirname(from_rel)
    for d in (base, 'psp', 'frontend-common', 'netdrv', '.'):
        p = os.path.normpath(os.path.join(d, inc)).replace('\\', '/')
        if os.path.exists(os.path.join(src, p)):
            return p
    return None


# ------------------------------------------------------------- logical files
_FRAG_INC = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+\.inc\.c)"[ \t]*$')


def logical_view(src):
    """A `*.inc.c` fragment is part of the file that #includes it (adversarial review
    2026-11-07: on a real P4 split R5 flagged fragments that use GPSP_NO_TELEMETRY
    without including fe_evt.h themselves, R1's base-phase hijack exemption only looked
    inside the file defining the hijack, and KNOWN rows keyed to psp/main_psp.c missed).
    -> (dir, origin): a mirror of the scanned tree where every host has its fragments
    expanded in place of their #include line (split.py's inverse: the text of the
    unsplit file, line for line) and
    the fragments themselves are gone; origin[(host, line)] = (file, line).  When no
    fragment is included anywhere: (src, {})."""
    import shutil
    import tempfile
    hosts = []
    for rel in files(src, ('.c',)):
        if rel.endswith('.inc.c'):
            continue
        txt = open(os.path.join(src, rel), encoding='utf-8', errors='replace').read()
        if any(_FRAG_INC.match(l) for l in txt.split('\n')):
            hosts.append(rel)
    if not hosts:
        return src, {}
    d = tempfile.mkdtemp(prefix='ci_greps_view.')
    for top in ('psp', 'frontend-common', 'netdrv', '.'):
        base = os.path.join(src, top)
        for root, dirs, fns in os.walk(base):
            if top == '.':
                dirs[:] = []
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
            for fn in fns:
                if fn.endswith(('.c', '.h', '.inc')):
                    p = os.path.join(root, fn)
                    q = os.path.join(d, os.path.relpath(p, src))
                    os.makedirs(os.path.dirname(q), exist_ok=True)
                    shutil.copyfile(p, q)
    origin, used = {}, set()

    def expand(rel, depth=0):
        out = []
        for k, ln in enumerate(open(os.path.join(src, rel), encoding='utf-8', errors='replace').read().split('\n'), 1):
            m = _FRAG_INC.match(ln)
            q = os.path.normpath(os.path.join(os.path.dirname(rel), m.group(1))).replace('\\', '/') if m else None
            if q and os.path.exists(os.path.join(src, q)) and depth < 8:
                used.add(q)
                frag = expand(q, depth + 1)
                if frag and frag[-1][0] == '':
                    frag = frag[:-1]      # the fragment's final newline ends the #include's line
                out.extend(frag)
            else:
                out.append((ln, (rel, k)))
        return out
    for h in hosts:
        lines = expand(h)
        with open(os.path.join(d, h), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(x for x, _ in lines))
        for i, (_, o) in enumerate(lines, 1):
            origin[(h, i)] = o
    for q in used:
        try:
            os.remove(os.path.join(d, q))
        except OSError:
            pass
    return d, origin


# ------------------------------------------------------------- driver
def run(src, phase, known_path, allow_path, pp_dir, only=None, logical=True, r6_path=None):
    view, origin = logical_view(src) if logical else (src, {})
    try:
        fails, out = _run(view, phase, known_path, allow_path, pp_dir, only, origin, r6_path)
    finally:
        if view != src:
            import shutil
            shutil.rmtree(view, ignore_errors=True)
    if origin:
        out.insert(0, 'logical files: %d *.inc.c fragment(s) read as part of their includer' %
                   len({o[0] for o in origin.values() if o[0].endswith('.inc.c')}))
    return fails, out


def _where(origin, h):
    o = origin.get((h.file, h.line))
    return '%s:%d%s' % (h.file, h.line, ' (%s:%d)' % o if o and o[0] != h.file else '')


def _run(src, phase, known_path, allow_path, pp_dir, only, origin, r6_path=None):
    known = load_known(known_path)
    allow = load_allow(allow_path)
    out, hits = [], []
    rules = only or ['R1', 'R2', 'R3', 'R4', 'R5', 'R6']
    if 'R1' in rules:
        h, info, extra = rule_r1(src, phase, known, pp_dir)
        hits += h
        out.append('R1 exits (phase %s): %d raw calls accepted as wrapped/whitelisted, %d violations'
                   % (phase, len(info), len(h)))
        out += extra
    if 'R2' in rules:
        h, n = rule_r2(src)
        hits += h
        out.append('R2 cold/noreturn: %d declarations, %d without section()' % (n, len(h)))
    if 'R3' in rules:
        h, n = rule_r3(src)
        hits += h
        out.append('R3 hooks: %d gpsp_* functions scanned, %d reach pwr_hold' % (n, len(h)))
    if 'R4' in rules:
        h, ns, nc = rule_r4(src, allow)
        hits += h
        out.append('R4 fe_evt args: %d call sites, %d calls in arguments, %d flagged' % (ns, nc, len(h)))
    if 'R5' in rules:
        h, n = rule_r5(src)
        hits += h
        out.append('R5 telemetry include order: %d files test GPSP_NO_TELEMETRY, %d too early' % (n, len(h)))
    if 'R6' in rules:
        pinned = load_r6(r6_path)
        h, n = rule_r6(src, pinned)
        hits += h
        out.append('R6 main-only functions: %d (%d pinned in %s), %d new without noinline + section()'
                   % (n, len(pinned), os.path.basename(r6_path or '-'), len(h)))
    fails = 0
    for h in hits:
        kn = [k for k in known if k['rule'] == h.rule and k['file'] == h.file and k['key'] == h.key
              and (not k['phase'] or k['phase'] == phase)]
        if kn:
            out.append('  KNOWN %s %s %s -- %s (%s)' % (h.rule, _where(origin, h), h.msg, kn[0]['reason'], h.key))
        else:
            fails += 1
            out.append('  FAIL  %s %s %s [%s]' % (h.rule, _where(origin, h), h.msg, h.key))
    out.append('T11: %s' % ('green' if not fails else '%d violations' % fails))
    return fails, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', required=True, help='gpsp root')
    ap.add_argument('--phase', default='base', choices=('base', 'P', 'HW2'))
    ap.add_argument('--known', default=os.path.join(HERE, 'ci_greps.known'))
    ap.add_argument('--allow', default=os.path.join(HERE, 'ci_greps.fe_evt_calls'))
    ap.add_argument('--pp', help='dir with main_psp.<flavour>.i (exit_matrix/aec_pspcc output)')
    ap.add_argument('--rules', help='comma list, default all')
    ap.add_argument('--write-allow', action='store_true')
    ap.add_argument('--r6', default=os.path.join(HERE, 'ci_greps.r6_pinned'))
    ap.add_argument('--write-r6', action='store_true',
                    help='write the R6 pinned list from this tree (the Epoch C start tree only)')
    ap.add_argument('--no-logical', action='store_true',
                    help='read *.inc.c fragments as files of their own (the pre-2026-11-07 view; a mutant)')
    o = ap.parse_args()
    if o.write_allow:
        print('wrote %d names to %s' % (write_allow(o.src, o.allow), o.allow))
        return 0
    if o.write_r6:
        print('wrote %d names to %s' % (write_r6(o.src, o.r6), o.r6))
        return 0
    fails, out = run(o.src, o.phase, o.known, o.allow, o.pp, o.rules.split(',') if o.rules else None,
                     logical=not o.no_logical, r6_path=o.r6)
    print('\n'.join(out))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
