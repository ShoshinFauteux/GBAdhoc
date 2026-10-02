#!/usr/bin/env python3
"""The GB link rig scorer must never pass a broken run (no false positives).

Builds a minimal, complete pair of console logs that score PASS, then breaks
one thing at a time -- a desync, a lost exit line, different binaries, a
dropped hash, a save file that is not the committed image, a missing line
-- and requires each to score FAIL or INVALID as appropriate."""
import os
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SCORER = os.path.join(HERE, '..', 'gblink', 'score_session.py')
SAVE = {r: bytes((i * (3 if r == 'host' else 5)) & 0xFF for i in range(32768))
        for r in ('host', 'join')}


CTL = 'EVT adhoc_ctl at=%s channel=6 bssid=%s ctl_peers=%d'
BSS1, BSS2 = '02:26:43:4b:be:68', '02:01:4a:48:61:1b'


def log(role):
    crc = '%08x' % (zlib.crc32(SAVE[role]) & 0xFFFFFFFF)
    other = 'join' if role == 'host' else 'host'
    return [
        'EVT build stamp=x',
        'EVT boot_ok',
        'EVT psp_model code=2 name=%s' % ('PSP-3000' if role == 'host' else 'PSP-1000'),
        'EVT rig run_id=7 arm=A',
        'EVT build eboot_crc=1234abcd size=100',
        'EVT cfg src=harness key=%s raw=1' % role,
        'EVT cfg_audit harness_keys=10 config_keys=3',
        'EVT ap_loaded steps=2 file=link.ap crc=0badf00d',
        'EVT gblink_create role=%s rom_size=1048576 save_size=32768'
        % ('host' if role == 'host' else 'guest'),
        'EVT gblink_own_rom sha1=%s size=1048576' % ('aa' * 20),
        'EVT gblink_partner_rom source=same_as_own',
        'EVT gblink_start slot=%d delay=4 seed=1790500000 partner=library'
        % (0 if role == 'host' else 1),
        'EVT ap_mark text=battle_start f=2773 t_ms=0 it=0',
        'EVT ap_mark text=battle_end f=6609 t_ms=0 it=0',
        'EVT gblink_stats slot=0 delay=4 f0=6600 f1=6599 stalls=35 streak_max=32 '
        'hashes=109/110 last_hash=6600:ef5c46344ae7fe8b',
        'EVT gblink_final frame=6885 hash=2c4ea7c5d6867d07 save=32768',
        'EVT gblink_save_commit size=32768 crc=%s ok=1' % crc,
        'EVT gblink_done frame=6885 hash=2c4ea7c5d6867d07 save_committed=32768 '
        'hashes=114/114 stalls=35',
        'EVT evt_drop total=0',
        'EVT exit code=0 reason=gblink_done',
    ], other


def run(t, host, join, host_sav=None, join_sav=None, extra=()):
    hp, jp = os.path.join(t, 'h.log'), os.path.join(t, 'j.log')
    with open(hp, 'w') as fh:
        fh.write('\n'.join(host) + '\n')
    with open(jp, 'w') as fh:
        fh.write('\n'.join(join) + '\n')
    args = [sys.executable, SCORER, hp, jp, '--host-model', 'PSP-3000',
            '--join-model', 'PSP-1000', '--expect-crc', '1234abcd',
            '--expect-delay', '4', '--expect-transfer', 'none', *extra]
    for role, data in (('host', host_sav), ('join', join_sav)):
        if data is not None:
            p = os.path.join(t, role + '.sav')
            with open(p, 'wb') as fh:
                fh.write(data)
            args += ['--%s-sav' % role, p]
    r = subprocess.run(args, capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    H, _ = log('host')
    J, _ = log('join')
    cases = [
        ('clean run', H, J, SAVE['host'], SAVE['join'], 0),
        ('desync on the join', H,
         J[:-3] + ['EVT gblink_desync frame=600 own=1 peer=2'] + J[-3:],
         None, None, 1),
        ('host log truncated (no exit)', H[:-1], J, None, None, 2),
        ('different binaries', H,
         [l.replace('1234abcd', '1234abce') for l in J], None, None, 2),
        ('one hash unmatched', [l.replace('114/114', '113/114') for l in H], J,
         None, None, 1),
        ('final hashes differ', H,
         [l.replace('hash=2c4ea7c5d6867d07', 'hash=2c4ea7c5d6867d08') for l in J],
         None, None, 1),
        ('log loss', [l.replace('total=0', 'total=3') for l in H], J,
         None, None, 2),
        ('an owner config.ini key only an older build read (listed, valid)',
         H + ['EVT cfg_unknown src=config key=me_sameframe raw=1'], J,
         None, None, 0),
        ('misspelled key', H +['EVT cfg_unknown src=harness key=gblnk_delay raw=4'],
         J, None, None, 2),
        ('roles swapped', H, [l.replace('role=guest', 'role=host') for l in J],
         None, None, 2),
        ('save on the card is not the committed one', H, J,
         SAVE['host'], SAVE['host'], 1),
        ('join session failed', H,
         [l for l in J if 'gblink_done' not in l and 'exit' not in l] +
         ['EVT gblink_failed reason=peer_lost slot=1 f0=1 f1=1',
          'EVT exit code=4 reason=gblink_failed'], None, None, 1),
        ('no gblink_done line (absence is not success)', H,
         [l for l in J if not l.startswith('EVT gblink_done')], None, None, 1),
        ('save never committed', H,
         [l for l in J if 'gblink_save_commit' not in l], None, None, 1),
        ('wrong console model', H,
         [l.replace('PSP-1000', 'PSP-3000') for l in J], None, None, 2),
        ('two sessions in one run', H + ['EVT gblink_start slot=0 delay=4 '
                                         'seed=1790500000 partner=library'],
         J, None, None, 2),
        ('delays differ', H, [l.replace('delay=4 seed', 'delay=5 seed') for l in J],
         None, None, 2),
        ('one ad-hoc cell on both (hw5 adhoc_ctl lines)',
         H + [CTL % ('up', BSS1, 1)], J + [CTL % ('up', BSS1, 1)],
         None, None, 0),
        ('a pass-shaped run in two cells (telemetry contradicts itself)',
         H + [CTL % ('up', BSS1, 1)], J + [CTL % ('up', BSS2, 1)],
         None, None, 1),
        ('the cell changed mid-run on one console',
         H + [CTL % ('up', BSS1, 1), CTL % ('hb', BSS2, 1)],
         J + [CTL % ('up', BSS1, 1)], None, None, 1),
    ]
    bad = 0
    with tempfile.TemporaryDirectory(prefix='gblink-score-') as t:
        for name, h, j, hs, js, want in cases:
            got, out = run(t, h, j, hs, js)
            if got != want:
                bad += 1
                print('FAIL scorer: %s -> exit %d, want %d\n%s'
                      % (name, got, want, out[-1500:]))
        # live link, resume and the reference's final frame:hash
        live = ['EVT gblink_live own_state=83357 peer_state=83357',
                'EVT gblink_resume how=live_end rc=0']
        lx = ('--live', '--expect-resume', 'live_end', '--expect-final',
              '6885:2c4ea7c5d6867d07')
        for name, h, j, want in (
                ('live: both linked from running games, went on, final == ref',
                 H + live, J + live, 0),
                ('live: the join powered on (no state)', H + live,
                 J + ['EVT gblink_live own_state=0 peer_state=83357',
                      'EVT gblink_resume how=live_end rc=0'], 1),
                ('live: the host restarted instead of going on',
                 H + ['EVT gblink_live own_state=83357 peer_state=83357',
                      'EVT gblink_resume how=restart rc=0'], J + live, 1),
                ('live: final hash differs from the reference',
                 [l.replace('hash=2c4ea7c5d6867d07', 'hash=2c4ea7c5d6867d08')
                  for l in H] + live,
                 [l.replace('hash=2c4ea7c5d6867d07', 'hash=2c4ea7c5d6867d08')
                  for l in J] + live, 1)):
            got, out = run(t, h, j, extra=lx)
            if got != want:
                bad += 1
                print('FAIL scorer: %s -> exit %d, want %d\n%s'
                      % (name, got, want, out[-1500:]))
            cases.append(name)
        marks =('--expect-marks', 'battle_start,battle_end')
        for name, h, j, want in (
                ('battle: both scripts reached start and end', H, J, 0),
                ('battle: the join never saw battle_end', H,
                 [l for l in J if 'battle_end' not in l], 1)):
            got, out = run(t, h, j, extra=marks)
            if got != want:
                bad += 1
                print('FAIL scorer: %s -> exit %d, want %d\n%s'
                      % (name, got, want, out[-1500:]))
            cases.append(name)
        # Root cause first: a script that stops ends its console's session,
        # and the partner then sees peer_lost -- that is not a link failure.
        cut = [l for l in J if not l.startswith(('EVT gblink_done', 'EVT exit',
                                                 'EVT gblink_save_commit',
                                                 'EVT gblink_final'))]
        hcut = [l for l in H if not l.startswith(('EVT gblink_done', 'EVT exit',
                                                  'EVT gblink_save_commit',
                                                  'EVT gblink_final'))]
        def nop(L, tail):
            return ([l for l in L if not l.startswith(('EVT gblink_',
                                                        'EVT ap_mark',
                                                        'EVT exit'))]
                    + ['EVT adhoc_stats tx=33 rx=0'] + tail
                    + ['EVT exit code=4 reason=gblink_failed'])
        for name, h, j, want in (
                ('root cause: the join script failed, the host lost its peer',
                 hcut + ['EVT gblink_failed reason=peer_lost slot=0 f0=2179 f1=2181',
                         'EVT exit code=4 reason=gblink_failed'],
                 cut + ['EVT ap_fail step=13 line=23 op=stepram frame=2178 it=0 '
                        'val=0x00000006',
                        'EVT gblink_failed reason=peer_lost slot=1 f0=2175 f1=2178',
                        'EVT exit code=3 reason=ap_fail'],
                 'ROOT CAUSE [SCRIPT]: join script failed: step 13 line 23'),
                ('root cause: the join left DONE before the host had its FINAL',
                 hcut + ['EVT gblink_failed reason=peer_lost slot=0 f0=1027 f1=1026',
                         'EVT exit code=4 reason=gblink_failed'], J,
                 'ROOT CAUSE [END_RACE]: join ended DONE'),
                ('root cause: a desync outranks the peer_lost that follows it',
                 hcut + ['EVT gblink_desync frame=600 own=1 peer=2',
                         'EVT gblink_failed reason=desync slot=0 f0=600 f1=600',
                         'EVT exit code=4 reason=gblink_failed'],
                 cut + ['EVT gblink_failed reason=peer_aborted slot=1 f0=600 f1=600',
                        'EVT exit code=4 reason=gblink_failed'],
                 'ROOT CAUSE [DESYNC]: host saw'),
                ('root cause: the consoles never heard each other (hw3 run 9)',
                 [l for l in H if not l.startswith(('EVT gblink_', 'EVT ap_mark',
                                                    'EVT exit'))]
                 + ['EVT adhoc_stats tx=140 rx=0',
                    'EVT gblink_failed reason=no_peer slot=0 f0=0 f1=0',
                    'EVT exit code=4 reason=gblink_failed'],
                 [l for l in J if not l.startswith(('EVT gblink_', 'EVT ap_mark',
                                                    'EVT exit'))]
                 + ['EVT adhoc_stats tx=280 rx=0',
                    'EVT exit code=0 reason=user'],
                 'ROOT CAUSE [NO_PEER]: the consoles never found each other'),
                ('NO_PEER named: two cells of one name (hw4 auto003)',
                 nop(H, [CTL % ('up', BSS1, 0),
                         'EVT gblink_failed reason=no_peer slot=0 f0=0 f1=0']),
                 nop(J, ['EVT adhoc_join group=GBLNK7 rc=0 tries=3 channel=6 '
                         'bssid=' + BSS1, CTL % ('up', BSS2, 0),
                         'EVT gblink_failed reason=no_peer slot=1 f0=0 f1=0']),
                 'SPLIT_CELL: host bssid %s ch 6, join bssid %s' % (BSS1, BSS2)),
                ('NO_PEER named: the join never saw the group',
                 nop(H, [CTL % ('up', BSS1, 0),
                         'EVT gblink_failed reason=no_peer slot=0 f0=0 f1=0']),
                 nop(J, ['EVT adhoc_join group=GBLNK7 rc=-15 tries=20 '
                         'channel=0 bssid=00:00:00:00:00:00',
                         'EVT gblink_auto_fail rc=-15 stage=join_no_group']),
                 "NO_GROUP: the join never saw the host's group in 20 scans"),
                ('NO_PEER named: one cell, nothing crossed',
                 nop(H, [CTL % ('up', BSS1, 1),
                         'EVT gblink_failed reason=no_peer slot=0 f0=0 f1=0']),
                 nop(J, [CTL % ('up', BSS1, 1),
                         'EVT gblink_failed reason=no_peer slot=1 f0=0 f1=0']),
                 'SAME_CELL: bssid %s ch 6, adhocctl peers host=1 join=1' % BSS1),
                ('NO_PEER from an older build: the cell is unknown, not guessed',
                 nop(H, ['EVT gblink_failed reason=no_peer slot=0 f0=0 f1=0']),
                 nop(J, ['EVT gblink_failed reason=no_peer slot=1 f0=0 f1=0']),
                 'CELL_UNKNOWN: no adhoc_ctl line on host and join')):
            got, out = run(t, h, j)
            if got != 1 or want not in out:
                bad += 1
                print('FAIL scorer: %s -> exit %d, no %r\n%s'
                      % (name, got, want, out[-1500:]))
            cases.append(name)
    if bad:
        return 1
    print('gblink scorer: %d cases, a clean run passes and every broken one '
          'is FAIL or INVALID' % len(cases))
    return 0


if __name__ == '__main__':
    sys.exit(main())
