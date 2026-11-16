#!/usr/bin/env python3
"""p0a_test_srcscan.py MAIN_C -- T13 ef_srcscan P0a repair: every built flavour defines
GBADHOC_MGIFT.  On a tree with `#ifdef GBADHOC_MGIFT` blocks the release scan must hold
the mgift functions; the mutant (the define dropped, the pre-P0a CONFIGS) loses them.
On a tree without MGIFT blocks (01067e74) the define changes nothing: also checked."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import ef_srcscan as S  # noqa: E402

src = sys.argv[1]
has = '#ifdef GBADHOC_MGIFT' in open(src, encoding='utf-8', errors='replace').read()
fl = ('release', 'lab', 'harness')
missing = [f for f in fl if f not in S.CONFIGS or not S.CONFIGS[f].get('GBADHOC_MGIFT')]
r = S.scan(src, fl)
live = {f: {x['name'] for x in r['functions'] if x[f]['active_at_open']} for f in fl}
S.RELEASE.pop('GBADHOC_MGIFT')
r2 = S.scan(src, ('release',))
live2 = {x['name'] for x in r2['functions'] if x['release']['active_at_open']}
lost = sorted(live['release'] - live2)
print('flavours %s define GBADHOC_MGIFT%s' % ('/'.join(fl), '' if not missing else ' EXCEPT %s' % missing))
print('release functions %d (lab %d, harness %d); mutant without the define: %d, lost %s'
      % (len(live['release']), len(live['lab']), len(live['harness']), len(live2),
         ' '.join(lost) or 'none'))
ok = not missing and ((has and 'mgift_game_ok' in lost) or (not has and not lost))
print('T13: %s' % ('ok' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
