#!/usr/bin/env python3
"""setup_gblink_cards.py on fake cards: it must never write into a USB window
it cannot date, never touch the handoff, and never write a card whose
ROLE.TXT names another console.

A fake card is a directory tree (PSP/GAME/GBADHOC/roms with the owner's
cartridges); a "parked" card also has GBADHOC-GBLINK/handoff/{RESULT,WINDOW}.TXT,
and a thread plays the console: it opens a new window (new token) after a
delay.  Checked:
  - a fresh card gets everything; a second run writes nothing (idempotent);
  - --dry-run writes nothing;
  - on a parked card nothing is written before the token changes, RESULT.TXT
    is untouched and no CMD.TXT appears (hw_loop must still see it parked);
  - a window too short for the margin: nothing written, exit 3 (rerun);
  - ROLE.TXT naming another model (drive letters drift): refused, no write.
"""
import hashlib
import importlib.util
import io
import os
import sys
import tempfile
import threading
import time
import contextlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'gblink', 'setup_gblink_cards.py')


def load():
    spec = importlib.util.spec_from_file_location('setup_gblink_cards', TOOL)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def w(p, data):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as fh:
        fh.write(data)


def run(mod, argv):
    out = io.StringIO()
    old = sys.argv
    sys.argv = ['setup_gblink_cards.py'] + argv
    try:
        with contextlib.redirect_stdout(out):
            try:
                rc = mod.main()
            except SystemExit as e:
                rc = e.code if isinstance(e.code, int) else ('exit', str(e.code))
    finally:
        sys.argv = old
    return rc, out.getvalue()


def main():
    mod = load()
    red, crystal = b'RED-' * 4096, b'CRYSTAL-' * 8192
    mod.CRYSTAL_MD5 = hashlib.md5(crystal).hexdigest()     # the fake Rev 1
    bad = []
    with tempfile.TemporaryDirectory(prefix='gblink-setup-') as td:
        rig = os.path.join(td, 'rig')
        w(os.path.join(rig, 'stage-A', 'EBOOT.PBP'), b'EBOOT' * 1000)
        w(os.path.join(rig, 'stage-A', 'gbadhoc_me.prx'), b'PRX')
        for r in ('host', 'join'):
            w(os.path.join(rig, 'stage-A', r + '-.gpsp-harness.ini'),
              ('%s = 1\n' % r).encode())
            w(os.path.join(rig, 'golden', r + '-red.gb.sav'), r.encode() * 9)
        w(os.path.join(rig, 'cards', 'roms', 'synth8m.gbc'), b'S' * 5000)
        w(os.path.join(rig, 'reference', 'gen1-rom.sha1'),
          hashlib.sha1(red).hexdigest().encode())
        backup = os.path.join(td, 'backup')
        w(os.path.join(backup, 'MANIFEST.verified'), b'ok')

        def card(name):
            c = os.path.join(td, name)
            g = os.path.join(c, 'PSP', 'GAME', 'GBADHOC', 'roms')
            w(os.path.join(g, 'Pokemon - Red Version (USA).gb'), red)
            w(os.path.join(g, 'Pokemon - Crystal Version (USA).gbc'), crystal)
            return c, os.path.join(c, 'PSP', 'GAME', 'GBADHOC-GBLINK')
        H, HA = card('H')
        J, JA = card('J')
        base = ['--backup-verified', backup, '--rig', rig, '--arm', 'A',
                '--window-wait', '20']
        cards = ['host=%s=PSP-3000' % H, 'join=%s=PSP-1000' % J]

        # dry run: nothing
        rc, out = run(mod, base + ['--dry-run'] + cards)
        if rc != 0 or os.path.exists(HA) or os.path.exists(JA):
            bad.append('dry run wrote something (rc %s)\n%s' % (rc, out))
        # fresh cards: everything; then idempotent
        rc, out = run(mod, base + cards)
        if rc != 0 or not os.path.isfile(os.path.join(HA, 'EBOOT.PBP')) or \
                open(os.path.join(JA, 'ROLE.TXT')).read() != 'join PSP-1000\n':
            bad.append('fresh install failed (rc %s)\n%s' % (rc, out))
        rc, out = run(mod, base + cards)
        if rc != 0 or out.count(' 0 of ') != 2:
            bad.append('second run was not a no-op\n%s' % out)

        # parked: a changed file may only land after the token changes
        w(os.path.join(rig, 'stage-A', 'EBOOT.PBP'), b'EBOOT2' * 1000)
        result = b'run=39\nexit=0\nreason=gblink_done\nstatus=parked\n'
        for A in (HA, JA):
            w(os.path.join(A, 'handoff', 'RESULT.TXT'), result)
            w(os.path.join(A, 'handoff', 'WINDOW.TXT'),
              b'token=39-70\nseconds=300\n')
        opened = {}

        def console(A, delay):
            time.sleep(delay)
            # what the card held while the old window (unknown age) was open
            with open(os.path.join(A, 'EBOOT.PBP'), 'rb') as fh:
                opened[A] = fh.read() == b'EBOOT2' * 1000
            w(os.path.join(A, 'handoff', 'WINDOW.TXT'),
              b'token=39-71\nseconds=300\n')
        ths = [threading.Thread(target=console, args=(HA, 1.5)),
               threading.Thread(target=console, args=(JA, 3.0))]
        for t in ths:
            t.start()
        rc, out = run(mod, base + cards)
        for t in ths:
            t.join()
        for A in (HA, JA):
            eb = os.path.join(A, 'EBOOT.PBP')
            if open(eb, 'rb').read() != b'EBOOT2' * 1000:
                bad.append('parked: %s not updated\n%s' % (A, out))
            elif opened[A]:
                bad.append('parked: %s written before its window opened' % A)
            if open(os.path.join(A, 'handoff', 'RESULT.TXT'), 'rb').read() != result:
                bad.append('parked: RESULT.TXT touched on %s' % A)
            if os.path.exists(os.path.join(A, 'handoff', 'CMD.TXT')):
                bad.append('parked: CMD.TXT written on %s' % A)
        if rc != 0:
            bad.append('parked install rc %s\n%s' % (rc, out))

        # a window shorter than the margin: nothing written, rerun asked for
        w(os.path.join(rig, 'stage-A', 'EBOOT.PBP'), b'EBOOT3' * 1000)
        th = threading.Thread(target=lambda: (time.sleep(1.0), w(os.path.join(
            HA, 'handoff', 'WINDOW.TXT'), b'token=40-1\nseconds=10\n')))
        th.start()
        rc, out = run(mod, base + ['--window-margin', '30'] + cards)
        th.join()
        if rc != 3 or open(os.path.join(HA, 'EBOOT.PBP'), 'rb').read() != \
                b'EBOOT2' * 1000 or 'rerun' not in out:
            bad.append('short window: rc %s, wrote anyway?\n%s' % (rc, out))

        # ROLE.TXT of another model: refused before any write
        rc, out = run(mod, base + ['host=%s=PSP-1000' % H,
                                   'join=%s=PSP-3000' % J])
        if rc == 0 or 'drive letters drift' not in str(rc) + out:
            bad.append('model mismatch not refused: rc %s\n%s' % (rc, out))
    for b in bad:
        print('FAIL setup: ' + b)
    if bad:
        return 1
    print('gblink setup: dry run, fresh + idempotent install, parked window '
          'discipline, short window, model guard all pass')
    return 0


if __name__ == '__main__':
    sys.exit(main())
