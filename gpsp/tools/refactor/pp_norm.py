#!/usr/bin/env python3
"""pp_norm.py -- the token view of pp_diff.sh (T3 / G0): stdin (a `cpp -E -P`
stream) -> stdout with every run of whitespace OUTSIDE string and character
literals collapsed to one space (leading/trailing trimmed), and every literal
copied byte for byte.

Why not `tr -s`: whitespace INSIDE a literal is part of the token.  The tr
view made "a  b" equal to "a b" and a TAB in a string equal to a space, so a
.rodata change passed G0 (adversarial review 2026-11-05, mutants M6/M7 of
tests/source-split_test_pp_diff.sh).

Bytes are read as latin-1, so any input round-trips.
"""
import sys

WS = ' \t\n\r\f\v'


def norm(s):
    out = []
    i, n = 0, len(s)
    pend = False
    while i < n:
        c = s[i]
        if c in WS:
            pend = True
            i += 1
            continue
        if pend and out:
            out.append(' ')
        pend = False
        if c == '"' or c == "'":
            j = i + 1
            while j < n and s[j] != c and s[j] != '\n':
                if s[j] == '\\':
                    j += 1
                j += 1
            out.append(s[i:j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def main():
    data = sys.stdin.buffer.read().decode('latin-1')
    sys.stdout.buffer.write(norm(data).encode('latin-1'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
