#!/usr/bin/env python3
"""score_rig.py -- score every cycle of a GB link rig session (hw_loop logs).

    python tools/gblink/score_rig.py --logs <hw_loop --logs dir> --rig <prepare_gblink_rig.py out>
        [--host-model PSP-3000] [--join-model PSP-1000]

For each cycle NNN with both autoNNN-<arm>-host.log and -join.log it calls
score_session.py with the arm's expectations (the trade oracle and fixtures
for A/B/L, the golden for B/L, the transfers for X/W/Y), the manifest's
EBOOT CRC, the staged scripts (I5) and the console models (I8), and prints
one line per cycle plus a table of the numbers that matter: stalls, the
longest stall streak, cartridge transfer times and the running heap census.
Exit 0 only when every cycle PASSes.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SAVE = {'A': ('red.gb.sav', 'red.gb.sav'), 'B': ('crystal.gbc.sav',) * 2,
        'C': ('red.gb.sav', 'red.gb.sav'), 'D': ('crystal.gbc.sav',) * 2,
        'E': ('red.gb.sav', 'red.gb.sav'),
        'V': ('crystal.gbc.sav',) * 2, 'U': ('crystal.gbc.sav',) * 2,
        'R': ('crystal.gbc.sav',) * 2, 'Q': ('crystal.gbc.sav',) * 2,
        'L': ('crystal.gbc.sav',) * 2, 'T': ('crystal.gbc.sav',) * 2,
        'P': ('crystal.gbc.sav',) * 2, 'K': ('crystal.gbc.sav',) * 2,
        'X': ('crystal.gbc.sav', 'red.gb.sav'),
        'W': ('crystal.gbc.sav', 'red.gb.sav'),
        'Y': ('synth8m.gbc.sav', 'red.gb.sav'),
        'N': ('crystal.gbc.sav',) * 2, 'J': ('crystal.gbc.sav',) * 2,
        'H': ('crystal.gbc.sav',) * 2, 'I': (None, None)}
SCRIPT = {'A': ('gen1_host.ap', 'gen1_guest.ap'),
          'B': ('gen2_host.ap', 'gen2_guest.ap'),
          'C': ('gen1_battle_host.ap', 'gen1_battle_guest.ap'),
          'D': ('gen2_host.ap', 'gen2_guest.ap'),
          'E': ('gen1_battle_host.ap', 'gen1_battle_guest.ap'),
          'V': ('gen2_live_host.ap', 'gen2_live_guest.ap'),
          'U': ('gen2_live_host.ap', 'gen2_live_guest.ap'),
          'R': ('gen2_reload_host.ap', 'gen2_reload_guest.ap'),
          'Q': ('gen2_powerclock_host.ap', 'gen2_powerclock_guest.ap'),
          'L': ('gen2_host.ap', 'gen2_guest.ap'),
          'T': ('gen2_host.ap', 'gen2_guest.ap'),
          'P': ('gen2_host.ap', 'gen2_guest.ap'),
          'K': ('gen2_host.ap', 'gen2_guest.ap'),
          'X': ('play.ap',) * 2, 'W': ('play.ap',) * 2, 'Y': ('play.ap',) * 2,
          'N': ('play.ap',) * 2, 'J': ('play.ap',) * 2, 'H': ('play.ap',) * 2}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--logs', required=True)
    ap.add_argument('--rig', required=True)
    ap.add_argument('--host-model', default='PSP-3000')
    ap.add_argument('--join-model', default='PSP-1000')
    a = ap.parse_args()
    man = open(os.path.join(a.rig, 'MANIFEST.txt')).read()
    crc = re.search(r'EBOOT crc32 ([0-9a-f]{8})', man).group(1)
    cycles = {}
    for p in glob.glob(os.path.join(a.logs, 'auto*-*-host.log')):
        m = re.match(r'auto(\d+)-(\w+)-host\.log$', os.path.basename(p))
        if m:
            cycles[int(m.group(1))] = m.group(2)
    if not cycles:
        print('no autoNNN-<arm>-host.log in %s' % a.logs)
        return 2
    worst = 0
    rows = []
    roots = []
    for n in sorted(cycles):
        arm = cycles[n]
        if arm not in SAVE:
            # hw_loop names the setup run 'x' (it staged no arm for it); the
            # console's own log says which arm the card was set up with.
            with open(os.path.join(a.logs, 'auto%03d-%s-host.log' % (n, arm)),
                      errors='replace') as fh:
                m = re.search(r'^EVT rig run_id=\S+ arm=(\w+)', fh.read(), re.M)
            if m and m.group(1) in SAVE:
                print('auto%03d: hw_loop arm %s, the log says arm %s'
                      % (n, arm, m.group(1)))
                arm = m.group(1)
        if arm not in SAVE:
            # The setup run carries arm A (setup_gblink_cards.py --arm).
            print('auto%03d arm %s: unknown arm, not scored' % (n, arm))
            worst = max(worst, 2)
            continue
        if arm == 'I':
            # the geometry probe: no session; each console's log is read by
            # icprobe_geometry.py (0 geometry, 2 none, 3 contradictory)
            code = 0
            for role in ('host', 'join'):
                lp = os.path.join(a.logs, 'auto%03d-I-%s.log' % (n, role))
                r = subprocess.run([sys.executable, os.path.join(
                    HERE, '..', 'cachemap', 'icprobe_geometry.py'), lp],
                    capture_output=True, text=True)
                print('auto%03d arm I %s: %s' % (n, role, (r.stdout.strip()
                      .splitlines() or ['no output'])[0]))
                code = max(code, 0 if r.returncode == 0 else 1)
            worst = max(worst, code)
            continue
        hs, js = SAVE[arm]
        sh, sj = SCRIPT[arm]
        args = [sys.executable, os.path.join(HERE, 'score_session.py'),
                '--logs', a.logs, '--cycle', str(n), '--json',
                '--expect-crc', crc, '--expect-delay', '4',
                '--host-model', a.host_model, '--join-model', a.join_model,
                '--host-script', os.path.join(a.rig, 'scripts', sh),
                '--join-script', os.path.join(a.rig, 'scripts', sj)]
        pre = 'auto%03d-' % n
        args += ['--host-sav', os.path.join(a.logs, pre + 'host-' + hs),
                 '--join-sav', os.path.join(a.logs, pre + 'join-' + js)]
        g = os.path.join(a.rig, 'golden')
        ref = os.path.join(a.rig, 'reference')

        def reference(tag):
            return ['--golden-a', os.path.join(ref, tag + '-d4-a.sav'),
                    '--golden-b', os.path.join(ref, tag + '-d4-b.sav')]
        if arm == 'A':
            args += ['--gen', 'gen1', '--fix-a', os.path.join(g, 'host-red.gb.sav'),
                     '--fix-b', os.path.join(g, 'join-red.gb.sav'),
                     '--slot-a', '0', '--slot-b', '0', '--expect-transfer',
                     'none'] + reference('gen1')
        elif arm in ('C', 'E'):
            args += ['--expect-marks', 'battle_start,battle_end',
                     '--expect-transfer', 'none'] + reference('battle')
        elif arm == 'Q':
            args += ['--gen', 'gen2',
                     '--fix-a', os.path.join(g, 'host-crystal.gbc.sav'),
                     '--fix-b', os.path.join(g, 'join-crystal.gbc.sav'),
                     '--slot-a', '0', '--slot-b', '2', '--expect-transfer',
                     'none', '--expect-marks', 'clock_ok',
                     '--expect-resume', 'boot_from_battery']
        elif arm in ('V', 'U', 'R'):
            args += ['--gen', 'gen2',
                     '--fix-a', os.path.join(g, 'host-crystal.gbc.sav'),
                     '--fix-b', os.path.join(g, 'join-crystal.gbc.sav'),
                     '--slot-a', '0', '--slot-b', '2', '--expect-transfer',
                     'none', '--live']
            if arm == 'R':
                args += ['--expect-marks', 'clock_ok',
                         '--expect-resume', 'boot_from_battery']
            else:
                with open(os.path.join(ref, 'live-d4.final')) as fh:
                    fin = fh.read().strip()
                args += ['--expect-marks', 'resumed,played_on',
                         '--expect-resume', 'live_end',
                         '--expect-final', fin] + reference('live')
        elif arm in ('B', 'D', 'L', 'T', 'P', 'K'):
            # Reproducible since the harness freezes the cartridge clock at
            # the seed on both consoles (hw1's Gen 2 failure).
            args += ['--gen', 'gen2',
                     '--fix-a', os.path.join(g, 'host-crystal.gbc.sav'),
                     '--fix-b', os.path.join(g, 'join-crystal.gbc.sav'),
                     '--slot-a', '0', '--slot-b', '2', '--expect-transfer',
                     'none'] + reference('gen2')
        elif arm in ('N', 'J', 'H'):
            # bring-up stress: both consoles hold Crystal, nothing crosses
            args += ['--expect-transfer', 'none']
        else:
            args += ['--expect-transfer', 'both']
        r = subprocess.run(args, capture_output=True, text=True)
        try:
            res = json.loads(r.stdout)
        except ValueError:
            print('auto%03d arm %s: scorer crashed: %s' % (n, arm, r.stderr[-400:]))
            worst = 2
            continue
        worst = max(worst, r.returncode)
        why = '; '.join(res['invalid'] or res['fail'])
        if res.get('root_cause'):
            why = '%s: %s  [then: %s]' % (res['root_cause'][0],
                                          res['root_cause'][1], why)
        print('auto%03d arm %s: %s%s' % (n, arm, res['verdict'],
                                        (' -- ' + why) if why else ''))
        m = res['metrics']
        rows.append((n, arm, res['verdict'], m))
        roots.append((n, arm, res.get('root_cause'),
                      [c for c in res.get('checks', [])
                       if 'one ad-hoc cell' in c or 'ad-hoc cell not' in c]))
    print()
    print('cycle arm verdict  host stalls/streak  join stalls/streak  '
          'host rx cart             join rx cart             running heap free (host/join)')
    for n, arm, v, m in rows:
        h, j = m.get('host', {}), m.get('join', {})
        heap = lambda x: (x.get('heap') or {}).get('gblink_running', ('unknown',))[0]
        print('%5d %3s %-8s %8s/%-9s %8s/%-9s %-24s %-24s %s/%s'
              % (n, arm, v, h.get('stalls'), h.get('stall_streak_max'),
                 j.get('stalls'), j.get('stall_streak_max'),
                 h.get('rom_received'), j.get('rom_received'),
                 heap(h), heap(j)))
    # hw5: did any relaunch fail to find its partner, and was every run in
    # one ad-hoc cell?
    nopeer = [(n, arm, rc[1]) for n, arm, rc, _c in roots
              if rc and rc[0] == 'NO_PEER']
    cells = [c for _n, _a, _r, cs in roots for c in cs]
    one = sum(1 for c in cells if c.startswith('ok ') and 'one ad-hoc cell' in c)
    print('\nbring-up: %d runs (each a relaunch), NO_PEER %d; one ad-hoc cell '
          'confirmed in %d, contradicted in %d, not logged in %d'
          % (len(roots), len(nopeer), one,
             sum(1 for c in cells if c.startswith('FAIL')),
             sum(1 for c in cells if 'not logged' in c)))
    for n, arm, why in nopeer:
        print('  auto%03d %s NO_PEER: %s' % (n, arm, why))
    scans = [(n, m.get('join', {}).get('join_scan')) for n, _a, _v, m in rows]
    print('join scans before it joined: %s' % ', '.join(
        '%03d:%s' % x for x in scans))
    print('\nending on each console\'s own clock (ms; end_at>resume = from the '
          'end frame being set to the game back):')
    keys = ('end_at>final', 'final>done', 'done>screen', 'screen>linger',
            'linger>close', 'close>resume', 'end_at>resume')
    print('cycle arm role  ' + ' '.join('%13s' % k for k in keys))
    for n, arm, _v, m in rows:
        for role in ('host', 'join'):
            e = m.get(role, {}).get('end_ms') or {}
            if e:
                print('%5d %3s %-5s ' % (n, arm, role) +
                      ' '.join('%13s' % e.get(k, '-') for k in keys))
    print('\nRIG VERDICT %s' % {0: 'PASS', 1: 'FAIL', 2: 'INVALID'}[worst])
    return worst


if __name__ == '__main__':
    sys.exit(main())
