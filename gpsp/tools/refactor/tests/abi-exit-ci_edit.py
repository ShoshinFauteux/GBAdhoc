#!/usr/bin/env python3
"""abi-exit-ci_edit.py SRC DST EDIT -- test helper: read SRC as a list of lines `L`,
run the Python statement(s) EDIT on it (they must assert what they expect to find),
write DST.  Used to make mutant COPIES; never pointed at a tracked file by the tests."""
import sys

src, dst, edit = sys.argv[1], sys.argv[2], sys.argv[3]
L = open(src, encoding='utf-8', errors='surrogateescape').read().split('\n')
exec(edit)
open(dst, 'w', encoding='utf-8', errors='surrogateescape', newline='\n').write('\n'.join(L))
