#!/usr/bin/env python3
"""score_session.py -- score ONE two-console GB link run from its two logs.

    score_session.py HOST.log JOIN.log [options]
    score_session.py --logs DIR --cycle N [options]     (hw_loop's naming)

The GB link rig's scorer (docs/RIG-DOUBLE-BATTLE.md standards, adapted to a
link session).  A run is first checked for VALIDITY -- the measurement and
the setup are what they claim to be -- and only a valid run is scored:

  I1  both logs exist, each has `EVT boot_ok` and an `EVT exit` line
  I2  `rig run_id/arm` identical on both (and equal to the meta files)
  I3  `build eboot_crc` identical on both (= --expect-crc, = meta crc32)
  I4  no `cfg_unknown` harness key; the role key (host = 1 / join = 1)
      echoed.  Unknown keys in config.ini (the owner's own file, e.g. one an
      older build read) are listed, not held against the run
  I5  `ap_loaded crc` equals the staged script's CRC32 (--host-script ...)
  I6  no log loss: `evt_drop total=0` on both (and RESULT.TXT evt_drop=0)
  I8  roles by content: `gblink_create role=host|guest` on the right log,
      `psp_model` equal to --host-model / --join-model when given
  I9  exactly one `gblink_start` per console, same delay and seed on both
      (= --expect-delay when given)
  I10 cartridges: each console's SHA-1 of the partner's cartridge equals
      the partner's own (and --host-rom-sha1 / --join-rom-sha1)

A VALID run PASSES when, on both consoles: the session ended DONE
(`gblink_done`, `exit code=0 reason=gblink_done`), every periodic hash
matched (`hashes=N/N`, N covering the run), no desync / final mismatch /
failure line exists, the final frame and hash are equal across consoles, the
save was committed (`gblink_save_commit ok=1`), and -- when the post-run
saves are given -- each save file IS the committed image (CRC of its first
`size` bytes) and the trade is exact (score_trade.py against the starting
fixtures) and, with --golden-*, equal to the one-process golden; with
--expect-marks, both scripts reached those `evt` marks (a battle's start
and end).

No conclusions from absence: a metric whose line is missing prints
`unknown`, never 0.  Exit status: 0 PASS, 1 FAIL, 2 INVALID.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
KV = re.compile(r'(\w+)=("[^"]*"|\S+)')


def parse(path):
    """-> list of (name, {k: v}) for every EVT line, in order."""
    out = []
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if not line.startswith('EVT '):
                continue
            body = line[4:].rstrip('\n')
            name, _, rest = body.partition(' ')
            kv = {k: v.strip('"') for k, v in KV.findall(rest)}
            kv['_raw'] = rest
            out.append((name, kv))
    return out


def evts(log, name):
    return [kv for n, kv in log if n == name]


def last(log, name, key=None):
    e = evts(log, name)
    if not e:
        return None
    return e[-1] if key is None else e[-1].get(key)


def crc32_file(path, n=None):
    with open(path, 'rb') as fh:
        d = fh.read()
    if n is not None:
        d = d[:n]
    return '%08x' % (zlib.crc32(d) & 0xFFFFFFFF), len(d)


class Score:
    def __init__(self):
        self.invalid = []
        self.fail = []
        self.notes = []
        self.root = None           # (category, text) of the first failure

    def inv(self, ok, what):
        self.notes.append(('ok  ' if ok else 'BAD ') + what)
        if not ok:
            self.invalid.append(what)

    def chk(self, ok, what):
        self.notes.append(('ok  ' if ok else 'FAIL ') + what)
        if not ok:
            self.fail.append(what)


def cell(L):
    """(bssid, channel, ctl_peers max) of the ad-hoc cell this console was in
    (adhoc_ctl lines, b417514 on), or None when the build does not log it."""
    e = evts(L, 'adhoc_ctl')
    if not e:
        return None
    peers = [int(x.get('ctl_peers', '-1')) for x in e
             if re.match(r'-?\d+$', x.get('ctl_peers', ''))]
    return (e[0].get('bssid'), e[0].get('channel'), max(peers) if peers else -1,
            len({x.get('bssid') for x in e}))


def no_peer_why(H, J):
    """Which of the ways two consoles can miss each other this was.
    SPLIT_CELL: two cells of one name (hw3 run 9, hw4 auto001/003: the join's
    connect-by-name created its own).  NO_GROUP: the join never saw the host's
    group (b417514: it no longer creates one).  SAME_CELL: one cell, and the
    radio still carried nothing (ports / PDP)."""
    j = last(J, 'adhoc_join')
    if j and j.get('rc') not in (None, '0'):
        return ("NO_GROUP: the join never saw the host's group in %s scans "
                "over %s ms" % (j.get('tries'), j.get('ms', 'unknown'))
                if j.get('rc') == '-15'
                else "JOIN_FAILED: the join's bring-up returned rc=%s"
                % j.get('rc'))
    ch, cj = cell(H), cell(J)
    if ch is None or cj is None:
        return 'CELL_UNKNOWN: no adhoc_ctl line on %s' % (
            ' and '.join(r for r, c in (('host', ch), ('join', cj)) if c is None))
    if ch[0] != cj[0]:
        return ('SPLIT_CELL: host bssid %s ch %s, join bssid %s ch %s'
                % (ch[0], ch[1], cj[0], cj[1]))
    return ('SAME_CELL: bssid %s ch %s, adhocctl peers host=%d join=%d, yet '
            'no datagram crossed' % (ch[0], ch[1], ch[2], cj[2]))


def root_cause(H, J):
    """The failure the others follow from, so a script that stops (and so
    ends its console's session) is never reported as a link failure.
    Categories: NO_PEER, SCRIPT, DESYNC, SETUP, END_RACE, LINK, EXIT."""
    L = {'host': H, 'join': J}
    other = {'host': 'join', 'join': 'host'}

    # The two consoles never heard each other (hw3 run 9: each created its
    # own ad-hoc group of the same name) -- everything else follows from it.
    heard = {r: bool(evts(L[r], 'peer_connected') or evts(L[r], 'gblink_hello')
                     or evts(L[r], 'gblink_start'))
             for r in ('host', 'join')}
    nopeer = [r for r in ('host', 'join')
              if (last(L[r], 'gblink_failed') or {}).get('reason') == 'no_peer']
    if nopeer or not (heard['host'] or heard['join']):
        rx = {r: (last(L[r], 'adhoc_stats') or {}).get('rx', '?')
              for r in ('host', 'join')}
        return ('NO_PEER', 'the consoles never found each other: %s (%s; '
                'ad-hoc rx host=%s join=%s; join: %s)'
                % (no_peer_why(H, J),
                   'timed out: ' + ', '.join(nopeer) if nopeer
                   else 'no peer_connected on either side', rx['host'],
                   rx['join'], (last(L['join'], 'adhoc_join') or
                    last(L['join'], 'adhoc_join_scan') or {})
                   .get('_raw', 'not logged')))

    def fate(role):
        f = last(L[role], 'gblink_failed')
        if f:
            return 'its session failed (%s)' % f.get('reason')
        if last(L[role], 'gblink_done'):
            return 'its session ended DONE'
        ex = last(L[role], 'exit')
        return 'it exited (%s)' % (ex.get('_raw') if ex else 'no exit line')

    for role in ('host', 'join'):
        d = last(L[role], 'gblink_desync')
        if d:
            return ('DESYNC', '%s saw the machines disagree at frame %s '
                    '(own %s, peer %s)' % (role, d.get('frame'), d.get('own'),
                                           d.get('peer')))
    fails = []
    for role in ('host', 'join'):
        for e in evts(L[role], 'ap_fail'):
            fails.append((int(e.get('frame', '0')), role, e))
    if fails:
        fails.sort(key=lambda x: x[0])
        f, role, e = fails[0]
        return ('SCRIPT', '%s script failed: step %s line %s (%s) at frame %s, '
                'value %s; %s: %s' % (role, e.get('step'), e.get('line'),
                                      e.get('op'), e.get('frame'), e.get('val'),
                                      other[role], fate(other[role])))
    for role in ('host', 'join'):
        f = last(L[role], 'gblink_failed')
        if f and f.get('reason') not in ('peer_lost', 'peer_aborted'):
            return ('SETUP' if f.get('reason') in ('rom_missing', 'rom_bad',
                                                    'memory', 'start', 'proto')
                    else 'LINK', '%s session failed: %s at f0=%s; %s: %s'
                    % (role, f.get('reason'), f.get('f0'), other[role],
                       fate(other[role])))
    for role in ('host', 'join'):
        f = last(L[role], 'gblink_failed')
        o = other[role]
        if f and f.get('reason') == 'peer_lost' and last(L[o], 'gblink_done'):
            return ('END_RACE', '%s ended DONE and left before %s had its '
                    'FINAL; %s failed peer_lost at f0=%s (no save written)'
                    % (o, role, role, f.get('f0')))
    lost = [r for r in ('host', 'join')
            if (last(L[r], 'gblink_failed') or {}).get('reason') == 'peer_lost']
    if lost:
        return ('LINK', 'the radio link was lost (%s: peer_lost)'
                % ', '.join(lost))
    for role in ('host', 'join'):
        ex = last(L[role], 'exit')
        if not ex or ex.get('code') != '0':
            return ('EXIT', '%s: %s' % (role, ex.get('_raw') if ex
                                        else 'no exit line'))
    return None


def end_ms(L):
    """The ending on this console's own clock (t_ms=, hw5 builds on): from
    the end frame being settled to the game being back.  Keys missing from
    older builds are absent, never 0."""
    def t(name):
        e = last(L, name)
        v = e.get('t_ms') if e else None
        return int(v) if v and v.isdigit() else None
    pts = [('end_at', t('gblink_end_at')), ('final', t('gblink_final')),
           ('done', t('gblink_done')), ('screen', t('gblink_ended_screen')),
           ('linger', t('gblink_linger')), ('close', t('gblink_close')),
           ('resume', t('gblink_resume'))]
    out = {}
    for (a, ta), (b, tb) in zip(pts, pts[1:]):
        if ta is not None and tb is not None:
            out['%s>%s' % (a, b)] = tb - ta
    if pts[0][1] is not None and pts[-1][1] is not None:
        out['end_at>resume'] = pts[-1][1] - pts[0][1]
    return out


def resolve_cycle(d, cycle):
    """hw_loop names: autoNNN-<arm>-<role>.log/.meta.json and
    autoNNN-<role>-<save name>."""
    def one(pat):
        m = sorted(glob.glob(os.path.join(d, pat)))
        return m[0] if len(m) == 1 else None
    n = '%03d' % cycle
    r = {}
    for role in ('host', 'join'):
        r[role + '_log'] = one('auto%s-*-%s.log' % (n, role))
        r[role + '_meta'] = one('auto%s-*-%s.meta.json' % (n, role))
        r[role + '_saves'] = sorted(glob.glob(os.path.join(
            d, 'auto%s-%s-*.sav' % (n, role))))
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('host_log', nargs='?')
    ap.add_argument('join_log', nargs='?')
    ap.add_argument('--logs')
    ap.add_argument('--cycle', type=int)
    ap.add_argument('--host-meta')
    ap.add_argument('--join-meta')
    ap.add_argument('--host-model')
    ap.add_argument('--join-model')
    ap.add_argument('--expect-crc')
    ap.add_argument('--expect-delay', type=int)
    ap.add_argument('--host-script')
    ap.add_argument('--join-script')
    ap.add_argument('--host-rom-sha1')
    ap.add_argument('--join-rom-sha1')
    ap.add_argument('--expect-transfer', choices=('none', 'host', 'join', 'both'),
                    help='which console receives the partner cartridge over '
                         'the radio: host = the host receives the join\'s')
    ap.add_argument('--gen', choices=('gen1', 'gen2', 'none'), default='none',
                    help='score the trade in the saves (needs --fix-* and '
                         'the post-run saves)')
    ap.add_argument('--fix-a', help='host starting save (the golden)')
    ap.add_argument('--fix-b', help='join starting save (the golden)')
    ap.add_argument('--slot-a', type=int, default=0)
    ap.add_argument('--slot-b', type=int, default=0)
    ap.add_argument('--host-sav')
    ap.add_argument('--join-sav')
    ap.add_argument('--save-name', help='with --logs: the save file name on '
                    'the card (e.g. crystal.gbc.sav)')
    ap.add_argument('--expect-marks', help='comma-separated autopilot marks '
                    '(evt TEXT) the scripts on both consoles must reach, '
                    'e.g. battle_start,battle_end')
    ap.add_argument('--expect-final', help='frame:hash the session must end '
                    'on (from the reference run), e.g. 8636:d00a...')
    ap.add_argument('--max-stall-streak', type=int,
                    help='fail if either console stalled longer than this '
                         'many frames in a row')
    ap.add_argument('--live', action='store_true',
                    help='a live link: both consoles linked from running '
                         'games (gblink_live own_state and peer_state > 0)')
    ap.add_argument('--expect-resume', help='how each game went on after the '
                    'session (gblink_resume how=): live_end, '
                    'boot_from_battery, restore_prelink')
    ap.add_argument('--golden-a')
    ap.add_argument('--golden-b')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()

    if a.logs is not None:
        if a.cycle is None:
            ap.error('--logs needs --cycle')
        r = resolve_cycle(a.logs, a.cycle)
        a.host_log = a.host_log or r['host_log']
        a.join_log = a.join_log or r['join_log']
        a.host_meta = a.host_meta or r['host_meta']
        a.join_meta = a.join_meta or r['join_meta']
        if a.save_name:
            for role in ('host', 'join'):
                m = [p for p in r[role + '_saves']
                     if p.endswith('-' + a.save_name)]
                if m and not getattr(a, role + '_sav'):
                    setattr(a, role + '_sav', m[0])
    s = Score()
    logs = {}
    # ---- I1 ------------------------------------------------------------
    for role, p in (('host', a.host_log), ('join', a.join_log)):
        if not p or not os.path.isfile(p):
            s.inv(False, 'I1 %s log missing (%s)' % (role, p))
            continue
        logs[role] = parse(p)
        L = logs[role]
        s.inv(bool(evts(L, 'boot_ok')), 'I1 %s EVT boot_ok' % role)
        s.inv(bool(evts(L, 'exit')), 'I1 %s EVT exit (not truncated)' % role)
    if len(logs) != 2:
        return report(a, s, {}, 2)
    H, J = logs['host'], logs['join']
    metas = {}
    for role, p in (('host', a.host_meta), ('join', a.join_meta)):
        if p and os.path.isfile(p):
            with open(p) as fh:
                metas[role] = json.load(fh)

    # ---- I2 ------------------------------------------------------------
    rh, rj = last(H, 'rig'), last(J, 'rig')
    ids, idj = ([rh.get('run_id'), rh.get('arm')] if rh else [None, None],
                [rj.get('run_id'), rj.get('arm')] if rj else [None, None])
    s.inv(rh is not None and rj is not None and ids == idj,
          'I2 run_id/arm equal: host %s, join %s' % (ids, idj))
    for role, m in metas.items():
        want = ids[1]
        if m.get('arm') == 'x' and ids[0] == 'setup':
            # hw_loop's first cycle runs what setup_gblink_cards.py staged
            # (run_id = setup); it names that cycle's arm 'x'.
            s.notes.append('--  I2 %s: the setup run (hw_loop arm x, log '
                           'arm %s)' % (role, want))
            continue
        s.inv(m.get('arm') == want, 'I2 %s meta arm %s == log arm %s'
              % (role, m.get('arm'), want))

    # ---- I3 ------------------------------------------------------------
    ch, cj = last(H, 'build', 'eboot_crc'), last(J, 'build', 'eboot_crc')
    s.inv(ch is not None and ch == cj, 'I3 eboot_crc equal: %s / %s' % (ch, cj))
    if a.expect_crc:
        s.inv(ch == a.expect_crc.lower(), 'I3 eboot_crc == manifest %s'
              % a.expect_crc)
    for role, m in metas.items():
        c = (m.get('eboot_crc32') or '').lower()
        want = ch if role == 'host' else cj
        s.inv(c == want, 'I3 %s card EBOOT crc32 %s == log %s' % (role, c, want))

    # ---- I4 ------------------------------------------------------------
    for role, L in (('host', H), ('join', J)):
        unk = [u for u in evts(L, 'cfg_unknown') if u.get('src') != 'config']
        s.inv(not unk, 'I4 %s cfg_unknown harness keys: %s' % (
            role, ', '.join(u.get('key', '?') for u in unk) or 'none'))
        cu = [u.get('key', '?') for u in evts(L, 'cfg_unknown')
              if u.get('src') == 'config']
        if cu:
            s.notes.append('--  I4 %s config.ini keys this build does not read: '
                           '%s' % (role, ', '.join(cu)))
        keys = {(e.get('src'), e.get('key')): e.get('raw')
                for e in evts(L, 'cfg')}
        s.inv(keys.get(('harness', role)) == '1',
              'I4 %s harness key %s = 1 echoed' % (role, role))

    # ---- I5 ------------------------------------------------------------
    for role, L, sp in (('host', H, a.host_script), ('join', J, a.join_script)):
        got = last(L, 'ap_loaded', 'crc')
        if sp:
            want, _ = crc32_file(sp)
            s.inv(got == want, 'I5 %s ap_loaded crc %s == staged %s'
                  % (role, got, want))
        else:
            s.notes.append('--  I5 %s ap_loaded crc %s (no script given)'
                           % (role, got or 'unknown'))

    # ---- I6 ------------------------------------------------------------
    for role, L in (('host', H), ('join', J)):
        d = last(L, 'evt_drop', 'total')
        s.inv(d == '0', 'I6 %s evt_drop total=%s' % (role, d or 'unknown'))
        m = metas.get(role)
        if m:
            rd = (m.get('result') or {}).get('evt_drop')
            s.inv(rd == '0', 'I6 %s RESULT.TXT evt_drop=%s' % (role, rd))

    # ---- I8 ------------------------------------------------------------
    for role, L, model, want in (('host', H, a.host_model, 'host'),
                                 ('join', J, a.join_model, 'guest')):
        got = last(L, 'gblink_create', 'role')
        s.inv(got == want, 'I8 %s gblink_create role=%s' % (role, got))
        pm = last(L, 'psp_model', 'name')
        if model:
            s.inv(pm == model, 'I8 %s psp_model %s == %s' % (role, pm, model))
        else:
            s.notes.append('--  I8 %s psp_model %s' % (role, pm or 'unknown'))

    # ---- I9 ------------------------------------------------------------
    st = {}
    for role, L in (('host', H), ('join', J)):
        e = evts(L, 'gblink_start')
        s.inv(len(e) == 1, 'I9 %s exactly one gblink_start (%d)' % (role, len(e)))
        st[role] = e[0] if e else {}
    dh, dj = st['host'].get('delay'), st['join'].get('delay')
    sh, sj = st['host'].get('seed'), st['join'].get('seed')
    s.inv(dh is not None and dh == dj and sh == sj,
          'I9 delay %s/%s seed %s/%s equal' % (dh, dj, sh, sj))
    if a.expect_delay is not None:
        s.inv(dh == str(a.expect_delay), 'I9 delay %s == %d'
              % (dh, a.expect_delay))

    # ---- I10 -----------------------------------------------------------
    own = {r: last(L, 'gblink_own_rom', 'sha1') for r, L in (('host', H), ('join', J))}
    for role, L, other in (('host', H, 'join'), ('join', J, 'host')):
        p = last(L, 'gblink_partner_rom')
        src = p.get('source') if p else None
        if src == 'same_as_own':
            ok = own[role] is not None and own[role] == own[other]
            what = 'same_as_own'
        elif src == 'transfer':
            ok = p.get('sha1') == own[other] and own[other] is not None
            what = 'transfer sha1 %s' % p.get('sha1')
        elif src == 'library':
            ok = own[other] is not None
            what = 'library %s' % p.get('path')
        else:
            ok, what = False, 'unknown'
        s.inv(ok, 'I10 %s partner cartridge (%s) == %s own %s'
              % (role, what, other, own[other]))
    for role, want in (('host', a.host_rom_sha1), ('join', a.join_rom_sha1)):
        if want:
            s.inv(own[role] == want.lower(), 'I10 %s cartridge sha1 %s == %s'
                  % (role, own[role], want))
    if a.expect_transfer:
        got = {r: (last(L, 'gblink_partner_rom', 'source') == 'transfer')
               for r, L in (('host', H), ('join', J))}
        want = {'none': (False, False), 'host': (True, False),
                'join': (False, True), 'both': (True, True)}[a.expect_transfer]
        s.inv((got['host'], got['join']) == want,
              'I10 transfers host=%s join=%s as expected (%s)'
              % (got['host'], got['join'], a.expect_transfer))

    # Consoles that never found each other never started a session, so the
    # session-shape invariants (I8-I10) cannot hold -- that is the failure
    # itself, not a run to throw away.  Identity checks still decide.
    if (root_cause(H, J) or ('',))[0] == 'NO_PEER':
        s.invalid = [n for n in s.invalid if not n.startswith(('I8 ', 'I9 ', 'I10 '))]
        s.fail.append('the consoles never found each other')
    valid = not s.invalid
    # ---- outcome -------------------------------------------------------
    fin = {}
    metrics = {}
    for role, L in (('host', H), ('join', J)):
        ex = last(L, 'exit')
        s.chk(ex is not None and ex.get('code') == '0' and
              ex.get('reason') == 'gblink_done',
              '%s exit %s' % (role, ex.get('_raw') if ex else 'unknown'))
        done = last(L, 'gblink_done')
        s.chk(done is not None, '%s gblink_done' % role)
        for bad in ('gblink_desync', 'gblink_final_mismatch', 'gblink_failed',
                    'gblink_peer_abort', 'gblink_input_gap',
                    'gblink_hello_refused', 'gblink_abandoned'):
            n = len(evts(L, bad))
            if n:
                s.chk(False, '%s %s x%d: %s' % (role, bad, n,
                                                 last(L, bad)['_raw'][:120]))
        if done:
            m = re.match(r'(\d+)/(\d+)', done.get('hashes', ''))
            hm, hs = (int(m.group(1)), int(m.group(2))) if m else (None, None)
            frame = int(done.get('frame', '0'))
            s.chk(hs is not None and hm == hs and hs >= max(1, frame // 60 - 2),
                  '%s hashes %s matched over %d frames' % (
                      role, done.get('hashes'), frame))
            fin[role] = (done.get('frame'), done.get('hash'))
        f = last(L, 'gblink_final')
        if f:
            fin.setdefault(role, (f.get('frame'), f.get('hash')))
        c = last(L, 'gblink_save_commit')
        s.chk(c is not None and c.get('ok') == '1',
              '%s save committed (%s)' % (role, c.get('_raw') if c else 'unknown'))
        stats = last(L, 'gblink_stats') or {}
        rx = last(L, 'gblink_rom_received')
        metrics[role] = {
            'stalls': stats.get('stalls', 'unknown'),
            'stall_streak_max': stats.get('streak_max', 'unknown'),
            # stutters: runs of stalls, and where the partner's inputs came
            # from (the redundant datagram / the ordered channel)
            'stall_episodes': stats.get('episodes', 'unknown'),
            'inputs_fast': stats.get('inputs_fast', 'unknown'),
            'inputs_ordered': stats.get('inputs_ordered', 'unknown'),
            'frames': '%s/%s' % (stats.get('f0', '?'), stats.get('f1', '?')),
            'rom_received': ('%s B in %.2f s' % (
                rx.get('size'), int(rx.get('us', 0)) / 1e6)) if rx else 'none',
            'heap': {e.get('at'): (e.get('free'), e.get('largest'))
                     for e in evts(L, 'heap_census')
                     if (e.get('at') or '').startswith(('gblink', 'post_load'))},
            'net': {k: (last(L, 'net_stats') or {}).get(k, 'unknown')
                    for k in ('retx', 'retx_pct', 'srtt_us', 'txq_hi', 'spill')},
            'end_deferred': len(evts(L, 'gblink_end_deferred')),
            'end_forced': len(evts(L, 'gblink_end_forced')),
            't_ms': ((metas.get(role) or {}).get('result') or {}).get('t_ms',
                                                                       'unknown'),
            'join_scan': ('%s@%sms' % ((last(L, 'adhoc_join') or {}).get(
                'tries', 'unknown'), (last(L, 'adhoc_join') or {}).get(
                'ms', '?')) if role == 'join' else '-'),
            'end_ms': end_ms(L),
        }
    # One ad-hoc cell: both consoles in the same BSS (b417514's adhoc_ctl).
    ch, cj = cell(H), cell(J)
    if ch is not None and cj is not None:
        s.chk(ch[0] == cj[0] and ch[3] == 1 and cj[3] == 1,
              'one ad-hoc cell: host bssid %s (%d seen), join bssid %s (%d seen)'
              % (ch[0], ch[3], cj[0], cj[3]))
    else:
        s.notes.append('--  ad-hoc cell not logged (host %s, join %s)'
                       % (ch and ch[0], cj and cj[0]))
    if 'host' in fin and 'join' in fin:
        s.chk(fin['host'] == fin['join'],
              'final frame/hash equal: host %s join %s' % (fin['host'], fin['join']))
    else:
        s.chk(False, 'final hash missing (%s)' % fin)

    if a.live:
        for role, L in (('host', H), ('join', J)):
            lv = last(L, 'gblink_live') or {}
            s.chk(int(lv.get('own_state', '0')) > 0 and
                  int(lv.get('peer_state', '0')) > 0,
                  '%s linked from running games (own_state=%s peer_state=%s)'
                  % (role, lv.get('own_state', 'unknown'),
                     lv.get('peer_state', 'unknown')))
    if a.expect_resume:
        for role, L in (('host', H), ('join', J)):
            rs = last(L, 'gblink_resume') or {}
            s.chk(rs.get('how') == a.expect_resume and rs.get('rc') == '0',
                  '%s went on after the session: how=%s rc=%s (want %s)'
                  % (role, rs.get('how', 'unknown'), rs.get('rc', 'unknown'),
                     a.expect_resume))
    if a.expect_final:
        want_f, _, want_h = a.expect_final.partition(':')
        for role in ('host', 'join'):
            got = fin.get(role)
            s.chk(got is not None and got[0] == want_f and got[1] == want_h,
                  '%s final frame:hash %s == reference %s' % (
                      role, '%s:%s' % got if got else 'unknown', a.expect_final))
    if a.max_stall_streak is not None:
        for role, m in metrics.items():
            v = m.get('stall_streak_max')
            s.chk(v not in (None, 'unknown') and int(v) <= a.max_stall_streak,
                  '%s longest stall %s frames <= %d' % (role, v,
                                                         a.max_stall_streak))
    if a.expect_marks:
        for role, L in (('host', H), ('join', J)):
            got = [e.get('text') for e in evts(L, 'ap_mark')]
            for mk in a.expect_marks.split(','):
                s.chk(mk in got, '%s script reached %s' % (role, mk))

    # ---- saves -----------------------------------------------------------
    for role, L, sav in (('host', H, a.host_sav), ('join', J, a.join_sav)):
        if not sav:
            continue
        c = last(L, 'gblink_save_commit')
        if not os.path.isfile(sav):
            s.chk(False, '%s post-run save missing: %s' % (role, sav))
            continue
        if c:
            got, n = crc32_file(sav, int(c.get('size', '0')))
            s.chk(got == c.get('crc') and n == int(c.get('size', '0')),
                  '%s save file is the committed image (crc %s == %s)'
                  % (role, got, c.get('crc')))
    if a.gen != 'none':
        if a.fix_a and a.fix_b and a.host_sav and a.join_sav:
            r = subprocess.run([sys.executable, os.path.join(HERE, 'score_trade.py'),
                                a.gen, a.fix_a, str(a.slot_a), a.fix_b,
                                str(a.slot_b), a.host_sav, a.join_sav],
                               capture_output=True, text=True)
            s.chk(r.returncode == 0, 'trade exact (score_trade.py): %s'
                  % (r.stdout.strip().splitlines() or ['no output'])[-1])
        else:
            s.chk(False, 'trade not scorable: need --fix-a/--fix-b and both saves')
    if a.golden_a or a.golden_b:
        def ram(p):
            d = open(p, 'rb').read()
            return d[:len(d) - 48] if len(d) % 256 == 48 else d
        for role, sav, g in (('host', a.host_sav, a.golden_a),
                             ('join', a.join_sav, a.golden_b)):
            if not g:
                continue
            if not sav or not os.path.isfile(sav) or not os.path.isfile(g):
                s.chk(False, '%s cartridge RAM vs reference %s: a file is '
                      'missing (%s)' % (role, os.path.basename(g), sav))
                continue
            s.chk(ram(sav) == ram(g), '%s cartridge RAM == reference %s'
                  % (role, os.path.basename(g)))
    code = 2 if not valid else (1 if s.fail else 0)
    if code:
        s.root = root_cause(H, J)
    return report(a, s, metrics, code)


def report(a, s, metrics, code):
    verdict = {0: 'PASS', 1: 'FAIL', 2: 'INVALID'}[code]
    if a.json:
        print(json.dumps({'verdict': verdict, 'root_cause': s.root,
                          'invalid': s.invalid,
                          'fail': s.fail, 'checks': s.notes,
                          'metrics': metrics}, indent=1))
    else:
        if s.root:
            print('ROOT CAUSE [%s]: %s' % s.root)
        for n in s.notes:
            print(n)
        for role, m in metrics.items():
            print('%s: stalls=%s streak_max=%s frames=%s rom_received=%s '
                  'end_deferred=%s end_forced=%s t_ms=%s'
                  % (role, m['stalls'], m['stall_streak_max'], m['frames'],
                     m['rom_received'], m['end_deferred'], m['end_forced'],
                     m['t_ms']))
            print('%s: stutter episodes=%s longest=%s inputs fast=%s '
                  'ordered=%s' % (role, m['stall_episodes'],
                                  m['stall_streak_max'], m['inputs_fast'],
                                  m['inputs_ordered']))
            print('%s: net %s' % (role, ' '.join('%s=%s' % kv
                                                 for kv in m['net'].items())))
            print('%s: heap %s' % (role, ' '.join(
                '%s=free:%s/largest:%s' % (k, v[0], v[1])
                for k, v in m['heap'].items()) or 'unknown'))
        if s.invalid:
            print('INVALID: ' + '; '.join(s.invalid))
        if s.fail:
            print('FAIL: ' + '; '.join(s.fail))
        if s.root:
            print('ROOT CAUSE [%s]: %s' % s.root)
        print('VERDICT %s' % verdict)
    return code


if __name__ == '__main__':
    sys.exit(main())
