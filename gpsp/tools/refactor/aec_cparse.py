#!/usr/bin/env python3
"""aec_cparse.py -- a small C statement parser for the abi-exit-ci tools (T8, T11).

Not a C compiler.  It tokenises one translation unit's own lines and parses FUNCTION
BODIES into statements (block / if / loop / switch / do / goto / return / label /
expression), which is enough to list every call in source order together with the
chain of conditions that guards it.  Two inputs:

  load_pp(path, fname)  `psp-gcc -E` output WITH linemarkers: only the lines whose
                        marker names `fname` are kept, each token carries its
                        ORIGINAL source line.  Macros are expanded, #if is resolved.
  load_raw(path)        the raw source: comments dropped, string literals kept,
                        preprocessor lines dropped; every #if branch is kept (like
                        scratch/exitseq.awk) and each token remembers the #if stack
                        it sat in (for the raw-vs-E classification).

API: Tok, tokenize_*, functions(toks) -> {name: (lo, hi)}, parse_body(toks, lo, hi)
     -> Block, walk_calls(node) -> [Call], Call has .name .line .args .conds .tok
"""
import re

KEYWORDS = {'if', 'else', 'for', 'while', 'do', 'switch', 'case', 'default', 'return',
            'goto', 'break', 'continue', 'sizeof', '__typeof__', 'typeof', '__attribute__',
            '__asm__', 'asm', '__extension__', '_Alignof', '__alignof__', 'defined',
            'void', 'char', 'short', 'int', 'long', 'unsigned', 'signed', 'float',
            'double', 'const', 'volatile', 'struct', 'union', 'enum', 'static', 'extern',
            '_Static_assert', '__builtin_offsetof', '__builtin_va_arg',
            '__builtin_types_compatible_p', '__builtin_expect'}
STMT_KW = {'if', 'for', 'while', 'do', 'switch', 'return', 'goto', 'break', 'continue',
           'case', 'default'}

_TOK = re.compile(r'''
   (?P<ws>\s+)
 | (?P<str>"(?:\\.|[^"\\])*")
 | (?P<chr>'(?:\\.|[^'\\])*')
 | (?P<id>[A-Za-z_]\w*)
 | (?P<num>\.?\d(?:[\w.]|[eEpP][+-])*)
 | (?P<op>->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=|\.\.\.|\#\#|[{}()\[\];,.?:~!%^&*+\-=<>|/\#])
''', re.X)


class FLine(int):
    """A source line of a FRAGMENT (psp/fe/X.inc.c) that main_psp.c includes: compares
    and hashes as (file, line), so a fragment's line 100 is never main_psp.c's line 100
    (P0a: T8's raw view and -E views key sites on (file, line) after the split).  Lines
    of main_psp.c itself stay plain ints, so a tree without fragments is unchanged.
    Prints as 'fe/X.inc.c:N'; '%d' gives the bare number."""
    def __new__(cls, n, file):
        o = int.__new__(cls, n)
        o.file = file
        return o

    def __eq__(self, other):
        if isinstance(other, FLine):
            return int(self) == int(other) and self.file == other.file
        return False if isinstance(other, int) else NotImplemented

    def __ne__(self, other):
        r = self.__eq__(other)
        return r if r is NotImplemented else not r

    def __hash__(self):
        return hash((int(self), self.file))

    def __str__(self):
        return '%s:%d' % (self.file, int(self))
    __repr__ = __str__


def norm_file(cur):
    """a linemarker / include path -> 'main_psp.c' or 'fe/X.inc.c' (relative to psp/)."""
    f = cur.replace('\\', '/')
    if '/psp/' in f:
        f = f[f.rindex('/psp/') + 5:]
    while f.startswith('./'):
        f = f[2:]
    return f


class Tok(object):
    __slots__ = ('kind', 'text', 'line', 'ifs', 'idx', 'pair', 'file')

    def __init__(self, kind, text, line, ifs=(), file=None):
        self.kind, self.text, self.ifs = kind, text, ifs
        self.file = file or 'main_psp.c'
        self.line = line if file is None or file == 'main_psp.c' or isinstance(line, FLine) \
            else FLine(line, file)
        self.idx, self.pair = -1, None


def link(toks):
    """Number the tokens and pair every bracket once (O(n)); unbalanced -> ValueError."""
    st = []
    for i, t in enumerate(toks):
        t.idx = i
        if t.text in ('(', '[', '{'):
            st.append(t)
        elif t.text in (')', ']', '}'):
            if not st:
                raise ValueError('unbalanced %s at line %d' % (t.text, t.line))
            o = st.pop()
            o.pair, t.pair = t, o
    if st:
        raise ValueError('unclosed %s at line %d' % (st[-1].text, st[-1].line))
    return toks

    def __repr__(self):
        return '%s@%d' % (self.text, self.line)


def _lex_line(text, line, ifs, out, file=None, alias=None):
    pos = 0
    while pos < len(text):
        m = _TOK.match(text, pos)
        if not m:            # stray char (backslash, @, `): skip it
            pos += 1
            continue
        pos = m.end()
        k = m.lastgroup
        if k == 'ws':
            continue
        t = m.group(k)
        if alias and k == 'id' and t in alias:
            t = alias[t]
        out.append(Tok(k, t, line, ifs, file))


_MARK = re.compile(r'^#\s*(\d+)\s+"([^"]*)"')


def _own_file(cur, fname):
    """The TU's own code: `fname` itself, plus any NON-header file it #includes (the
    PLAN §1.1 fragments psp/fe/<name>.inc.c).  Without the second part, every function
    moved into a fragment in phase P would silently vanish from the -E view (verifier
    finding, 2026-11-05).  At 01067e74 main_psp.c includes no such file, so the base
    figures are unchanged.  Fragment tokens keep their own file's line numbers."""
    if cur == fname or cur.endswith('/' + fname):
        return True
    return not cur.startswith('<') and cur.endswith(('.c', '.inc', '.inc.c'))


def load_pp(path, fname='main_psp.c'):
    """Tokens of the lines that came from `fname` (and the fragments it includes) in a
    -E (linemarker) output."""
    toks, cur, ln = [], None, 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            m = _MARK.match(raw)
            if m:
                ln, cur = int(m.group(1)), m.group(2)
                continue
            if raw.startswith('#'):          # #pragma etc.
                ln += 1
                continue
            if cur is not None and _own_file(cur, fname):
                nf = norm_file(cur)
                _lex_line(raw, ln, (), toks, None if nf == fname or nf.endswith('/' + fname) else nf)
            ln += 1
    return link(toks)


FRAG_INC = re.compile(r'^#include "(fe/[A-Za-z0-9_.]+\.inc\.c)"\s*$')


def raw_lines(path, follow=False, base=None, over=None):
    """[(file, line, text)] of a source file; with follow, every `#include "fe/X.inc.c"`
    line of main_psp.c is replaced by that fragment's lines (in place, in source order:
    the view split.py inverse gives), each line keeping its own file and line number.
    base: the psp/ directory the fragments are read from (default: the directory of
    `path`; a MUTATED COPY of the shell sits elsewhere and names the tree's psp/ here).
    over: {'fe/X.inc.c': host file}, a mutated copy read in place of that fragment."""
    import os
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    out = []
    base = base or os.path.dirname(os.path.abspath(path))
    for i, ln in enumerate(lines, 1):
        m = FRAG_INC.match(ln) if follow else None
        if m:
            fp = (over or {}).get(m.group(1)) or os.path.join(base, m.group(1))
            if not os.path.exists(fp):
                raise ValueError('%s:%d: fragment %s missing' % (path, i, fp))
            with open(fp, encoding='utf-8', errors='replace') as f:
                out += [(m.group(1), j, x) for j, x in enumerate(f.read().split('\n'), 1)]
        else:
            out.append((None, i, ln))
    return out


def load_raw(path, follow=False, alias=None, base=None, over=None):
    """Tokens of a raw C file: comments removed, directives removed, all #if branches
    kept.  tok.ifs = tuple of the enclosing #if/#ifdef/#ifndef/#elif/#else texts.
    follow: expand main_psp.c's `#include "fe/X.inc.c"` (raw_lines); fragment tokens
    carry tok.file and an FLine.  alias: {identifier: identifier} renames at lex time."""
    toks, stack = [], []
    in_cmt, cont = False, False
    for fl, i, raw in raw_lines(path, follow, base, over):
        s = raw
        if in_cmt:
            e = s.find('*/')
            if e < 0:
                continue
            s = ' ' * (e + 2) + s[e + 2:]
            in_cmt = False
        if cont:                             # continuation of a directive
            cont = s.rstrip().endswith('\\')
            continue
        st = s.lstrip()
        if st.startswith('#'):
            d = re.sub(r'/\*.*?\*/|//.*', '', st).strip()
            word = re.match(r'#\s*(\w*)', d).group(1)
            if word in ('if', 'ifdef', 'ifndef'):
                stack.append(d)
            elif word in ('elif', 'else'):
                if stack:
                    stack[-1] = stack[-1] + ' | ' + d
            elif word == 'endif':
                if stack:
                    stack.pop()
            cont = st.rstrip().endswith('\\')
            if '/*' in st and '*/' not in st[st.index('/*'):]:
                in_cmt = True
            continue
        # strip comments, keep strings
        out, j, n = [], 0, len(s)
        while j < n:
            c = s[j]
            if c == '"' or c == "'":
                k = j + 1
                while k < n and s[k] != c:
                    k += 2 if s[k] == '\\' else 1
                out.append(s[j:k + 1])
                j = k + 1
            elif s.startswith('/*', j):
                e = s.find('*/', j + 2)
                if e < 0:
                    in_cmt = True
                    break
                out.append(' ')
                j = e + 2
            elif s.startswith('//', j):
                break
            else:
                out.append(c)
                j += 1
        _lex_line(''.join(out), i, tuple(stack), toks, fl, alias)
    return link(toks)


def match_close(toks, i):
    """index (in `toks`) of the bracket closing toks[i] ((, [ or {)."""
    t0 = toks[i]
    if t0.pair is not None:
        return i + (t0.pair.idx - t0.idx)
    o = toks[i].text
    c = {'(': ')', '[': ']', '{': '}'}[o]
    d = 0
    for j in range(i, len(toks)):
        t = toks[j].text
        if t == o:
            d += 1
        elif t == c:
            d -= 1
            if d == 0:
                return j
    raise ValueError('unbalanced %s at line %d' % (o, toks[i].line))


def functions(toks):
    """{name: (open_brace_idx, close_brace_idx)} for every file-scope definition."""
    res, i, n = {}, 0, len(toks)
    while i < n:
        t = toks[i].text
        if t == '{':
            j = match_close(toks, i)
            k = i - 1
            # skip trailing attributes:  ) __attribute__((x)) {
            while k > 0 and toks[k].text == ')' and k >= 1:
                o = _match_open(toks, k)
                if o > 0 and toks[o - 1].text == '__attribute__':
                    k = o - 2
                    continue
                break
            if k > 0 and toks[k].text == ')':
                o = _match_open(toks, k)
                if o > 0 and toks[o - 1].kind == 'id' and toks[o - 1].text not in KEYWORDS:
                    res.setdefault(toks[o - 1].text, (i, j))
            i = j + 1
            continue
        i += 1
    return res


def _match_open(toks, j):
    if toks[j].pair is not None:
        return j - (toks[j].idx - toks[j].pair.idx)
    c = toks[j].text
    o = {')': '(', ']': '[', '}': '{'}[c]
    d = 0
    for k in range(j, -1, -1):
        t = toks[k].text
        if t == c:
            d += 1
        elif t == o:
            d -= 1
            if d == 0:
                return k
    return -1


# ---------------------------------------------------------------- statements
class Node(object):
    kind = '?'
    line = 0


class Block(Node):
    kind = 'block'

    def __init__(self, stmts, line):
        self.stmts, self.line = stmts, line


class If(Node):
    kind = 'if'

    def __init__(self, cond, then, els, line):
        self.cond, self.then, self.els, self.line = cond, then, els, line


class Loop(Node):
    kind = 'loop'

    def __init__(self, word, head, body, line, tail=None):
        self.word, self.head, self.body, self.line, self.tail = word, head, body, line, tail


class Simple(Node):
    """expression / declaration / return / goto / label / break / continue / case."""

    def __init__(self, kind, toks, line, label=None):
        self.kind, self.toks, self.line, self.label = kind, toks, line, label


class Parser(object):
    def __init__(self, toks, lo, hi):
        self.t, self.i, self.hi = toks, lo, hi

    def peek(self, k=0):
        j = self.i + k
        return self.t[j].text if j < self.hi else None

    def parse_block(self):
        assert self.t[self.i].text == '{'
        line = self.t[self.i].line
        end = match_close(self.t, self.i)
        self.i += 1
        stmts = []
        while self.i < end:
            stmts.append(self.parse_stmt(end))
        self.i = end + 1
        return Block(stmts, line)

    def paren(self):
        assert self.t[self.i].text == '(', (self.t[self.i], self.t[self.i].line)
        e = match_close(self.t, self.i)
        toks = self.t[self.i + 1:e]
        self.i = e + 1
        return toks

    def parse_stmt(self, end):
        tk = self.t[self.i]
        w, line = tk.text, tk.line
        if w == '{':
            return self.parse_block()
        if w == ';':
            self.i += 1
            return Simple('empty', [], line)
        if w == 'if':
            self.i += 1
            cond = self.paren()
            then = self.parse_stmt(end)
            els = None
            if self.i < end and self.peek() == 'else':
                self.i += 1
                els = self.parse_stmt(end)
            return If(cond, then, els, line)
        if w in ('while', 'for', 'switch'):
            self.i += 1
            head = self.paren()
            body = self.parse_stmt(end)
            return Loop(w, head, body, line)
        if w == 'do':
            self.i += 1
            body = self.parse_stmt(end)
            tail = []
            if self.i < end and self.peek() == 'while':
                self.i += 1
                tail = self.paren()
            if self.i < end and self.peek() == ';':
                self.i += 1
            return Loop('do', tail, body, line)
        if w in ('case', 'default'):
            j = self.i
            while j < end and self.t[j].text != ':':
                j += 1
            toks = self.t[self.i:j]
            self.i = j + 1
            return Simple('case', toks, line)
        if tk.kind == 'id' and self.peek(1) == ':' and w not in KEYWORDS:
            self.i += 2
            return Simple('label', [], line, label=w)
        kind = {'return': 'return', 'goto': 'goto', 'break': 'break',
                'continue': 'continue'}.get(w, 'expr')
        j, d = self.i, 0
        start = self.i
        stop = end                  # exclusive end of this statement's tokens
        while j < end:
            x = self.t[j].text
            if x in ('(', '[', '{'):
                d += 1
            elif x in (')', ']', '}'):
                d -= 1
            elif d == 0 and x == ';':
                stop = j + 1
                break
            elif d == 0 and j > start and x in STMT_KW and kind == 'expr' \
                    and self.t[j - 1].text in (')', '}'):
                stop = j            # a macro call without ';' ran into a statement
                break
            j += 1
        toks = self.t[self.i:stop]
        self.i = max(stop, self.i + 1)
        lab = None
        if kind == 'goto' and len(toks) > 1:
            lab = toks[1].text
        return Simple(kind, toks, line, label=lab)


def parse_body(toks, lo, hi):
    p = Parser(toks, lo, hi + 1)
    p.i = lo
    return p.parse_block()


# ---------------------------------------------------------------- calls
class Call(object):
    __slots__ = ('name', 'line', 'args', 'conds', 'tok', 'stmt')

    def __init__(self, name, line, args, conds, tok, stmt):
        self.name, self.line, self.args, self.conds, self.tok, self.stmt = \
            name, line, args, conds, tok, stmt

    def __repr__(self):
        return '%s@%d' % (self.name, self.line)


def text_of(toks):
    s = ' '.join(t.text for t in toks)
    s = re.sub(r'\s*([(),\[\];])\s*', r'\1', s)
    s = re.sub(r'([,;])', r'\1 ', s)
    return s.strip()


def calls_in(toks, conds=(), stmt=None):
    """Calls in a token list, in token order (an identifier followed by '(')."""
    out = []
    if toks and toks[0].text in ('extern', 'typedef'):
        return out                       # a block-scope declaration, not a call
    for k in range(len(toks) - 1):
        a = toks[k]
        if a.kind == 'id' and toks[k + 1].text == '(' and a.text not in KEYWORDS \
                and not (k > 0 and toks[k - 1].text in ('.', '->')):
            try:
                e = match_close(toks, k + 1)
                args = text_of(toks[k + 2:e])
            except ValueError:
                args = ''
            out.append(Call(a.text, a.line, args, conds, a, stmt))
    return out


def walk_calls(node, conds=()):
    """Every call under `node`, in source order; conds = guarding condition texts,
    outermost first ('!' + text for an else branch, 'loop:' + head for loop bodies)."""
    out = []
    if node is None:
        return out
    if isinstance(node, Block):
        for s in node.stmts:
            out += walk_calls(s, conds)
    elif isinstance(node, If):
        c = text_of(node.cond)
        out += calls_in(node.cond, conds, node)
        out += walk_calls(node.then, conds + (c,))
        if node.els is not None:
            out += walk_calls(node.els, conds + ('!(' + c + ')',))
    elif isinstance(node, Loop):
        h = text_of(node.head)
        if node.word == 'do':
            out += walk_calls(node.body, conds + ('do',))
            out += calls_in(node.head, conds, node)
        else:
            out += calls_in(node.head, conds, node)
            out += walk_calls(node.body, conds + ('%s:%s' % (node.word, h),))
    else:
        out += calls_in(node.toks, conds, node)
    return out


def blocks_containing(root, pred):
    """Yield (block, stmt_index) for every Block whose direct statement satisfies pred."""
    stack = [root]
    while stack:
        n = stack.pop()
        if isinstance(n, Block):
            for k, s in enumerate(n.stmts):
                if pred(s):
                    yield n, k
                stack.append(s)
        elif isinstance(n, If):
            stack += [x for x in (n.then, n.els) if x is not None]
        elif isinstance(n, Loop):
            stack.append(n.body)


# ---------------------------------------------------------------- helper inlining
# Epoch C (PLAN-EPOCHC-beta5 §3 C1) moves the pasted exit sequences into helpers
# (fe/teardown.inc.c) that take the site's ORDER and flags as compile-time constants.
# The tools read every site as the code it runs: each call statement of a named helper
# is replaced by the helper's body, with the parameters replaced by the call's argument
# tokens, every token carrying the CALL's line and file (so a site keeps one terminal
# line of its own, as before), and every `if (COND) { .. } [else { .. }]` whose COND is
# a constant (integer literals, the TU's enum constants, operators) after substitution
# reduced to the arm it takes (its braces dropped: the arm's statements join the
# caller's block, where the pasted copy had them).  A COND that reads anything else
# stays as it is.  Helpers are expanded inside each other (depth <= 8) and nowhere
# else: a helper's own definition is left in the token list.

_INT = re.compile(r'^(0[xX][0-9a-fA-F]+|\d+)[uUlL]*$')


def _const_eval(toks, consts):
    """The value of a constant integer expression, or None."""
    out = []
    for k, t in enumerate(toks):
        x = t.text
        if t.kind == 'num':
            m = _INT.match(x)
            if not m:
                return None
            out.append(str(int(m.group(1), 0)))
        elif t.kind == 'id':
            if x not in consts:
                return None
            out.append(str(consts[x]))
        elif x == '&&':
            out.append(' and ')
        elif x == '||':
            out.append(' or ')
        elif x == '!':
            out.append(' not ')
        elif x in ('(', ')', '&', '|', '^', '~', '<<', '>>', '+', '-', '*', '==', '!=',
                   '<', '>', '<=', '>='):
            out.append(x)
        else:
            return None
    try:
        v = eval(''.join(out), {'__builtins__': {}}, {})   # digits and operators only
    except Exception:
        return None
    return int(v) if isinstance(v, (bool, int)) else None


def enum_constants(toks):
    """{NAME: value} for the enumerators of the TU whose values are constant."""
    consts, i, n = {}, 0, len(toks)
    while i < n:
        if toks[i].text == 'enum':
            j = i + 1
            if j < n and toks[j].kind == 'id':
                j += 1
            if j < n and toks[j].text == '{':
                e = match_close(toks, j)
                k, nxt = j + 1, 0
                while k < e:
                    if toks[k].kind != 'id':
                        break
                    name = toks[k].text
                    k += 1
                    if k < e and toks[k].text == '=':
                        m, d = k + 1, 0
                        while m < e and not (d == 0 and toks[m].text == ','):
                            d += toks[m].text in ('(', '[')
                            d -= toks[m].text in (')', ']')
                            m += 1
                        v = _const_eval(toks[k + 1:m], consts)
                        k = m
                    else:
                        v = nxt
                    if v is None:
                        break
                    consts[name] = v
                    nxt = v + 1
                    if k < e and toks[k].text == ',':
                        k += 1
                i = e
        i += 1
    return consts


def _params(toks, lo):
    """Parameter names of the definition whose body opens at toks[lo]."""
    k = lo - 1
    while toks[k].text == ')':
        o = _match_open(toks, k)
        if o > 0 and toks[o - 1].text == '__attribute__':
            k = o - 2
            continue
        break
    o = _match_open(toks, k)
    names, cur, d = [], [], 0
    for t in toks[o + 1:k] + [Tok('op', ',', 0)]:
        if t.text == ',' and d == 0:
            ids = [x.text for x in cur if x.kind == 'id' and x.text not in KEYWORDS]
            if ids:
                names.append(ids[-1])
            cur = []
            continue
        d += t.text in ('(', '[')
        d -= t.text in (')', ']')
        cur.append(t)
    return names


def _split_args(toks):
    args, cur, d = [], [], 0
    for t in toks:
        if t.text == ',' and d == 0:
            args.append(cur)
            cur = []
            continue
        d += t.text in ('(', '[', '{')
        d -= t.text in (')', ']', '}')
        cur.append(t)
    if cur or args:
        args.append(cur)
    return args


def _retok(t, at):
    """A copy of t placed at the call token `at` (its line, file and #if stack)."""
    ifs = tuple(at.ifs) + tuple(x for x in t.ifs if x not in at.ifs)
    return Tok(t.kind, t.text, at.line, ifs, at.file if at.file != 'main_psp.c' else None)


def _fold(tl, consts):
    """Reduce every constant `if` of a token list (helper bodies: braced arms only)."""
    out, i, n = [], 0, len(tl)
    while i < n:
        t = tl[i]
        if t.text == 'if' and i + 1 < n and tl[i + 1].text == '(':
            c = _close(tl, i + 1)
            v = _const_eval(tl[i + 2:c], consts)
            if v is not None and c + 1 < n and tl[c + 1].text == '{':
                te = _close(tl, c + 1)
                then, els, j = tl[c + 2:te], [], te + 1
                if j < n and tl[j].text == 'else':
                    if j + 1 >= n or tl[j + 1].text != '{':
                        raise ValueError('inline: constant `if` at line %s: its else arm '
                                         'must be a braced block' % t.line)
                    ee = _close(tl, j + 1)
                    els, j = tl[j + 2:ee], ee + 1
                out += _fold(then if v else els, consts)
                i = j
                continue
        out.append(t)
        i += 1
    return out


def _close(tl, i):
    o = tl[i].text
    c = {'(': ')', '[': ']', '{': '}'}[o]
    d = 0
    for j in range(i, len(tl)):
        if tl[j].text == o:
            d += 1
        elif tl[j].text == c:
            d -= 1
            if d == 0:
                return j
    raise ValueError('inline: unbalanced %s at line %s' % (o, tl[i].line))


def inline_helpers(toks, names):
    """toks with every call statement `NAME(args);` of a helper in `names` expanded (see
    above).  Returns the input list itself when the TU defines none of them."""
    fns = functions(toks)
    have = [x for x in names if x in fns]
    if not have:
        return toks
    consts = enum_constants(toks)
    defs = {}
    for name in have:
        lo, hi = fns[name]
        defs[name] = (_params(toks, lo), toks[lo + 1:hi], lo, hi)
    inside = [(lo, hi) for (_, _, lo, hi) in defs.values()]

    def expand(tl, depth, skip=()):
        out, i, n, changed = [], 0, len(tl), False
        while i < n:
            t = tl[i]
            if (t.kind == 'id' and t.text in defs and i + 1 < n and tl[i + 1].text == '('
                    and not any(lo <= getattr(t, 'idx', -1) <= hi for lo, hi in skip)
                    and (i == 0 or tl[i - 1].text in (';', '{', '}', ')', 'else', ':'))):
                c = _close(tl, i + 1)
                if c + 1 < n and tl[c + 1].text == ';':
                    if depth > 8:
                        raise ValueError('inline: helpers nest deeper than 8 at line %s' % t.line)
                    params, body, _, _ = defs[t.text]
                    args = _split_args(tl[i + 2:c])
                    if args == [[]]:
                        args = []
                    if len(args) != len(params):
                        raise ValueError('inline: %s at line %s takes %d arguments, given %d'
                                         % (t.text, t.line, len(params), len(args)))
                    sub = dict(zip(params, args))
                    rep = []
                    for b in body:
                        if b.kind == 'id' and b.text in sub:
                            rep += [_retok(a, t) for a in sub[b.text]]
                        else:
                            rep.append(_retok(b, t))
                    rep = _fold(rep, consts)
                    if i > 0 and tl[i - 1].text in (')', 'else'):
                        rep = [Tok('op', '{', t.line, t.ifs, t.file if t.file != 'main_psp.c' else None)] + rep + \
                              [Tok('op', '}', t.line, t.ifs, t.file if t.file != 'main_psp.c' else None)]
                    out += expand(rep, depth + 1)[0]
                    i = c + 2
                    changed = True
                    continue
            out.append(t)
            i += 1
        return out, changed

    res, _ = expand(list(toks), 0, inside)
    return link(res)


def node_span(node):
    """(first_line, last_line) of a node."""
    lines = []

    def rec(n):
        if n is None:
            return
        lines.append(n.line)
        if isinstance(n, Block):
            for s in n.stmts:
                rec(s)
        elif isinstance(n, If):
            lines.extend(t.line for t in n.cond)
            rec(n.then)
            rec(n.els)
        elif isinstance(n, Loop):
            lines.extend(t.line for t in n.head)
            rec(n.body)
        else:
            lines.extend(t.line for t in n.toks)
    rec(node)
    return (min(lines), max(lines)) if lines else (0, 0)
