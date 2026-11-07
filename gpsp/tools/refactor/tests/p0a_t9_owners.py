#!/usr/bin/env python3
"""p0a_t9_owners.py CENSUS.json -- T9 owners of PLAN-P-beta5 §2.1 (P0a acceptance):
g_start_keepalive/grace/heard, g_reload_slow_ms, g_gblink_host_wait_peer = boot;
g_np_start_tid, s_link_bios = link; 0 UNASSIGNED; the MGIFT globals compiled into the
release define set; the seven C15 macro-globals tagged macro-in."""
import json
import sys

WANT = {'g_start_keepalive': 'boot', 'g_start_grace': 'boot', 'g_start_heard': 'boot',
        'g_reload_slow_ms': 'boot', 'g_gblink_host_wait_peer': 'boot',
        'g_np_start_tid': 'link', 's_link_bios': 'link'}
C15 = {'g_start_keepalive', 'g_start_grace', 'g_start_heard', 'g_reload_slow_ms',
       'g_gblink_host_wait_peer', 'g_mgift_consumed', 'g_mg_on'}

rows = [r for r in json.load(open(sys.argv[1])) if not r['extern']]
bad = []
got = {}
for r in rows:
    if r['name'] in WANT:
        got[r['name']] = r['owner']
        if r['owner'] != WANT[r['name']] or r.get('owner_how') != 'cutlist':
            bad.append('%s owner %s (%s), want %s' % (r['name'], r['owner'], r.get('owner_how'), WANT[r['name']]))
missing = sorted(set(WANT) - set(got))
un = [r['name'] for r in rows if r['owner'] is None]
mg = [r for r in rows if r['name'] in ('g_mg_on', 'g_mgift_consumed')]
mg_rel = [r['name'] for r in mg if not r['rel']]
macro = {r['name'] for r in rows if r.get('macro_in')}
print('owners: %s' % ', '.join('%s=%s' % kv for kv in sorted(got.items())))
print('macro-globals: %s' % ', '.join(sorted(macro)))
if bad or missing or un or mg_rel or not C15 <= macro:
    print('FAIL: %s %s unassigned=%s mgift-not-rel=%s c15-untagged=%s' % (
        bad, missing, un, mg_rel, sorted(C15 - macro)))
    sys.exit(1)
print('T9: %d definitions, 0 UNASSIGNED, §2.1 owners from the cutlist, MGIFT objects in the release set, '
      'C15 macro-globals tagged (%d found)' % (len(rows), len(macro)))
