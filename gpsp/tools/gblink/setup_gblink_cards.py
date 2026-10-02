#!/usr/bin/env python3
"""setup_gblink_cards.py -- create PSP/GAME/GBADHOC-GBLINK on each memory
stick for the GB link rig (and, with --m1, the three milestone-1 measurement
folders).  RUN ONLY AFTER THE OWNER HANDSHAKE: this is the first thing that
WRITES to a stick.  The lead runs it; the owner's PSP/GAME/GBADHOC is only
ever READ (to copy his own cartridges).

    python tools/gblink/setup_gblink_cards.py --backup-verified <backup dir>
        --rig <builds/gb-dual-out/rig> --arm A
        host=F:=PSP-3000 join=D:=PSP-1000
        [--rom-src <folder>] [--m1 <builds/gb-dual-out/m1>]

Writes, under PSP/GAME/GBADHOC-GBLINK/ on each card and nowhere else:
  EBOOT.PBP, gbadhoc_me.prx, .gpsp-harness.ini, link.ap, CONFIG.INI (the
  arm's stage, role-resolved, plus `run_id = setup`), roms/red.gb and
  roms/crystal.gbc (the owner's own cartridges, from this card's
  GBADHOC/roms or, when a card lacks one, from --rom-src), roms/synth8m.gbc,
  the golden saves (roms/*.sav), ROLE.TXT ("<role> <model>"), handoff/, log/.
With --m1 it also writes PSP/GAME/GBADHOC-GBLINK-{DUAL,DUAL8,DUALF,SOLO,SOLOH,GBA}/
(Crystal into the DUALs and SOLO, Emerald and gba_bios.bin into GBA, from the
same card),
and with --play PSP/GAME/GBADHOC-GBLINK-PLAY/ (the player build, the owner's
CONFIG.INI, both games, and the rig's golden saves as copies).

REFUSES: without <backup>/MANIFEST.verified; if a card's GBADHOC-GBLINK
already has a ROLE.TXT naming another role or another model (drive letters
drift); if Crystal is not the revision the scripts and golden were made with
(md5 301899b8...); if the Gen 1 cartridge is not the one the fixtures were
made on (<rig>/reference/gen1-rom.sha1); if the two cards' Gen 1 cartridges
differ.

PARKED CONSOLES (a rig console sitting in its USB handoff: the card has
GBADHOC-GBLINK/handoff/RESULT.TXT).  The console exports its card for a fixed
window (handoff/WINDOW.TXT: token, seconds) and then pulls it, whatever the
PC is doing.  So on such a card this tool writes NOTHING until it has seen the
window token CHANGE (a fresh window, dated by this PC), writes only files
whose content differs from the card's, stops before --window-margin seconds
of the window remain (rerun: it resumes, idempotent), and flushes the volume
cache (Write-VolumeCache) before returning.  It never touches handoff/ (no
CMD.TXT, RESULT.TXT kept, so hw_loop.py still sees the console parked and
wakes it) and never ejects.  --dry-run lists what would be written.
"""
import time
import argparse
import glob
import hashlib
import os
import shutil
import sys

APP = 'GBADHOC-GBLINK'
PLAY_APP = 'GBADHOC-GBLINK-PLAY'
CRYSTAL_MD5 = '301899b8087289a6436b0a241fbbb474'   # Crystal (USA, Europe) Rev 1
# The owner's file names (PSP/GAME/GBADHOC/roms), first match wins.
PATTERNS = {'red.gb': ('Pokemon - Red Version*.gb', 'Pokemon Red*.gb',
                       'Pokemon - Blue Version*.gb', 'Pokemon Blue*.gb'),
            'crystal.gbc': ('Pokemon - Crystal Version*.gbc',
                            'Pokemon Crystal*.gbc')}


def md5(p):
    h = hashlib.md5()
    with open(p, 'rb') as fh:
        for b in iter(lambda: fh.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def find_rom(dirs, name):
    for d in dirs:
        if not d or not os.path.isdir(d):
            continue
        for pat in PATTERNS[name]:
            m = sorted(glob.glob(os.path.join(d, pat)))
            if m:
                return m[0]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--backup-verified', required=True)
    ap.add_argument('--rig', required=True)
    ap.add_argument('--arm', default='A')
    ap.add_argument('--rom-src', help='folder holding the owner\'s Pokemon Red '
                    'and Crystal ROMs for a card that lacks them')
    ap.add_argument('--m1', help='also install the milestone-1 folders from here')
    ap.add_argument('--play', action='store_true',
                    help='also install GBADHOC-GBLINK-PLAY (the player build '
                         'from <rig>/play, the golden saves as copies)')
    ap.add_argument('--dry-run', action='store_true',
                    help='plan and check everything, write nothing')
    ap.add_argument('--window-margin', type=float, default=30,
                    help='parked card: stop writing when fewer seconds than '
                         'this remain in its USB window (default 30)')
    ap.add_argument('--window-wait', type=float, default=720,
                    help='parked card: how long to wait for a fresh window '
                         '(windows are at most 300 s; default 720)')
    ap.add_argument('cards', nargs=2, help='role=DRIVE:=MODEL')
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(a.backup_verified, 'MANIFEST.verified')):
        sys.exit('REFUSING: no verified backup at %s' % a.backup_verified)
    stage = os.path.join(a.rig, 'stage-' + a.arm)
    gold = os.path.join(a.rig, 'golden')
    synth = os.path.join(a.rig, 'cards', 'roms', 'synth8m.gbc')
    for p in (stage, gold, synth):
        if not os.path.exists(p):
            sys.exit('REFUSING: %s missing (run prepare_gblink_rig.py)' % p)

    want_sha1 = open(os.path.join(a.rig, 'reference',
                                  'gen1-rom.sha1')).read().strip()
    # Plan everything (and check every cartridge) before the first write.
    plan = []
    red_md5 = set()
    for spec in a.cards:
        role, _, rest = spec.partition('=')
        drive, _, model = rest.partition('=')
        drive = drive.rstrip('\\/')
        if role not in ('host', 'join') or not model:
            sys.exit('REFUSING: bad card spec %r' % spec)
        owner = os.path.join(drive + os.sep, 'PSP', 'GAME', 'GBADHOC')
        rig = os.path.join(drive + os.sep, 'PSP', 'GAME', APP)
        rolefile = os.path.join(rig, 'ROLE.TXT')
        if os.path.isfile(rolefile):
            have = open(rolefile).read().split()
            if not have or have[0] != role:
                sys.exit('REFUSING: %s already has %s for another role (%s)'
                         % (drive, APP, ' '.join(have)))
            if len(have) > 1 and have[1] != model:
                sys.exit('REFUSING: %s ROLE.TXT says %s is a %s, not a %s '
                         '(drive letters drift: check which console this is)'
                         % (drive, role, have[1], model))
        roms = {}
        for name in ('red.gb', 'crystal.gbc'):
            src = find_rom([os.path.join(owner, 'roms'), a.rom_src], name)
            if not src:
                sys.exit('REFUSING: no %s for %s (not in %s/roms; pass '
                         '--rom-src)' % (name, drive, owner))
            roms[name] = src
        if md5(roms['crystal.gbc']) != CRYSTAL_MD5:
            sys.exit('REFUSING: %s is not Crystal Rev 1 (md5 %s); the scripts '
                     'and the golden were made with that revision'
                     % (roms['crystal.gbc'], CRYSTAL_MD5))
        red_md5.add(md5(roms['red.gb']))
        with open(roms['red.gb'], 'rb') as fh:
            got = hashlib.sha1(fh.read()).hexdigest()
        if got != want_sha1:
            sys.exit('REFUSING: %s (sha1 %s) is not the cartridge the Gen 1 '
                     'fixtures were made on (%s): a Gen 1 save holds ROM '
                     'pointers, so a Blue save in Red (hw1) never reaches the '
                     'Cable Club.  Pass the right one with --rom-src, or '
                     'rebuild the fixtures on it (make_fixtures.sh)'
                     % (roms['red.gb'], got, want_sha1))
        plan.append((role, drive, model, owner, rig, roms))
    if len(red_md5) != 1:
        sys.exit('REFUSING: the two cards hold different Gen 1 cartridges '
                 '(%s); give both the same one via --rom-src' % sorted(red_md5))

    for role, drive, model, owner, rig, roms in plan:
        ops = []                 # (dst, src path or bytes)

        def put(dst, src):
            ops.append((dst, src))
        for f in sorted(os.listdir(stage)):
            src = os.path.join(stage, f)
            if f.startswith(('host-', 'join-')):
                if not f.startswith(role + '-'):
                    continue
                f = f[len(role) + 1:]
            if f == '.gpsp-harness.ini':
                with open(src, 'rb') as fh:
                    put(os.path.join(rig, f), fh.read() +
                        ('\nrun_id = setup\narm = %s\n' % a.arm).encode())
            else:
                put(os.path.join(rig, f), src)
        for name, src in roms.items():
            put(os.path.join(rig, 'roms', name), src)
        put(os.path.join(rig, 'roms', 'synth8m.gbc'), synth)
        for g in sorted(os.listdir(gold)):
            if g.startswith(role + '-'):
                put(os.path.join(rig, 'roms', g[len(role) + 1:]),
                    os.path.join(gold, g))
        put(os.path.join(rig, 'ROLE.TXT'), ('%s %s\n' % (role, model)).encode())
        game = os.path.join(drive + os.sep, 'PSP', 'GAME')
        if a.m1:
            for sub in ('GBADHOC-GBLINK-DUAL', 'GBADHOC-GBLINK-DUAL8',
                        'GBADHOC-GBLINK-DUALF', 'GBADHOC-GBLINK-SOLO',
                        'GBADHOC-GBLINK-SOLOH', 'GBADHOC-GBLINK-GBA'):
                srcd = os.path.join(a.m1, sub)
                for cur, _ds, fs in os.walk(srcd):
                    for f in sorted(fs):
                        put(os.path.join(game, sub, os.path.relpath(
                            os.path.join(cur, f), srcd)), os.path.join(cur, f))
                if sub != 'GBADHOC-GBLINK-GBA':
                    put(os.path.join(game, sub, 'roms', 'crystal.gbc'),
                        roms['crystal.gbc'])
            em = sorted(glob.glob(os.path.join(owner, 'roms',
                                               'Pokemon - Emerald Version*.gba')))
            gba = os.path.join(game, 'GBADHOC-GBLINK-GBA')
            if em and os.path.isfile(os.path.join(owner, 'gba_bios.bin')):
                put(os.path.join(gba, 'roms', 'emerald.gba'), em[0])
                put(os.path.join(gba, 'gba_bios.bin'),
                    os.path.join(owner, 'gba_bios.bin'))
            else:
                print('%s: NO Emerald/BIOS in %s: the GBA control will not run '
                      'on this card' % (drive, owner))
        play = os.path.join(a.rig, 'play')
        if a.play and os.path.isdir(play):
            # The player build for trying the menus by hand: the owner's own
            # settings, both games, and the rig's golden saves (COPIES --
            # his own GBADHOC saves are never touched).
            dst = os.path.join(game, PLAY_APP)
            for f in ('EBOOT.PBP', 'gbadhoc_me.prx'):
                put(os.path.join(dst, f), os.path.join(play, f))
            cfg = os.path.join(owner, 'CONFIG.INI')
            if os.path.isfile(cfg):
                put(os.path.join(dst, 'CONFIG.INI'), cfg)
            for name, src in roms.items():
                put(os.path.join(dst, 'roms', name), src)
            for g in sorted(os.listdir(gold)):
                if (g.startswith(role + '-') and 'synth' not in g and
                        g.endswith('.sav')):
                    put(os.path.join(dst, 'roms', g[len(role) + 1:]),
                        os.path.join(gold, g))
        if not write_card(drive, rig, ops, a):
            return 3
        print('%s: %s %s (role=%s model=%s arm=%s red=%s%s%s)'
              % (drive, APP, 'checked (dry run: nothing written)' if a.dry_run
                 else 'ready', role, model, a.arm,
                 os.path.basename(roms['red.gb']),
                 ', milestone-1 folders' if a.m1 else '',
                 ', ' + PLAY_APP if a.play and os.path.isdir(play) else ''))
    return 0


def content(src):
    if isinstance(src, bytes):
        return src
    with open(src, 'rb') as fh:
        return fh.read()


def same(dst, src):
    """The card already holds exactly this file (size, then bytes)."""
    try:
        if os.path.getsize(dst) != (len(src) if isinstance(src, bytes)
                                    else os.path.getsize(src)):
            return False
        with open(dst, 'rb') as fh:
            return fh.read() == content(src)
    except OSError:
        return False


def window(rig):
    try:
        with open(os.path.join(rig, 'handoff', 'WINDOW.TXT'),
                  errors='replace') as fh:
            kv = dict(l.split('=', 1) for l in fh.read().split() if '=' in l)
        return kv.get('token'), int(kv.get('seconds', '0'))
    except (OSError, ValueError):
        return None, 0


def flush(drive):
    if not (len(drive) == 2 and drive[1] == ':'):
        return True                         # a directory (tests): nothing to do
    import subprocess
    r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive',
                        '-Command', 'Write-VolumeCache -DriveLetter %s'
                        % drive[0]], capture_output=True, text=True)
    return r.returncode == 0


def write_card(drive, rig, ops, a):
    """Write ops onto one card; on a parked rig console, only inside a fresh
    USB window with time to spare."""
    todo = [(d, s) for d, s in ops if not same(d, s)]
    size = sum(len(s) if isinstance(s, bytes) else os.path.getsize(s)
               for _d, s in todo)
    print('%s: %d of %d files differ (%.1f MB)%s'
          % (drive, len(todo), len(ops), size / 1e6,
             ''.join('\n    ' + os.path.relpath(d, drive + os.sep)
                     for d, _s in todo)))
    if a.dry_run or not todo:
        return True
    parked = os.path.isfile(os.path.join(rig, 'handoff', 'RESULT.TXT'))
    deadline = None
    if parked:
        tok0, _sec = window(rig)
        if tok0 is None:
            print('REFUSING: %s has a handoff but no handoff/WINDOW.TXT: the USB '
                  'window cannot be bounded' % drive)
            return False
        print('%s: a parked rig console (window %s); waiting for a fresh USB '
              'window before writing...' % (drive, tok0))
        give_up = time.time() + a.window_wait
        while True:
            if time.time() > give_up:
                print('REFUSING: %s: no fresh window within %ds' % (drive,
                                                                   a.window_wait))
                return False
            tok, sec = window(rig)
            if tok is not None and tok != tok0:
                break
            time.sleep(0.5)
        deadline = time.time() + sec - a.window_margin
        print('%s: window %s (%ds) opened; writing until %ds before it closes'
              % (drive, tok, sec, a.window_margin))
    for d in ('roms', 'log', 'handoff'):
        os.makedirs(os.path.join(rig, d), exist_ok=True)
    done = 0
    for dst, src in todo:
        if deadline is not None and time.time() > deadline:
            break
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if isinstance(src, bytes):
            with open(dst, 'wb') as fh:
                fh.write(src)
        else:
            shutil.copy2(src, dst)
        done += 1
    ok = flush(drive)
    if not ok:
        print('%s: FLUSH FAILED -- do not let the window close on this; rerun '
              'setup (it only rewrites what differs)' % drive)
        return False
    if done < len(todo):
        print('%s: window nearly over after %d of %d files -- flushed; rerun '
              'the same command to finish (it resumes)' % (drive, done, len(todo)))
        return False
    return True


if __name__ == '__main__':
    sys.exit(main())
