#!/usr/bin/env python3
"""prepare_gblink_rig.py -- build the GB link rig's stages and golden set on
the PC.  Writes nothing to any memory stick (setup_gblink_cards.py does that,
after the owner handshake).  The GB-link counterpart of tools/rig/prepare_rig.py
(docs/RIG-DOUBLE-BATTLE.md standards), driven by tools/rig/hw_loop.py.

    python tools/gblink/prepare_gblink_rig.py --build <dir: EBOOT.PBP + gbadhoc_me.prx>
        --fix <make_fixtures.sh output ON THE RIG'S GEN 1 CARTRIDGE (Red)>
        --reference <make_references.sh output from the same fixtures>
        --backup <stick backup dir with F-PSP-3000/ and D-PSP-1000/>
        --out <builds/gb-dual-out/rig>

Roles are fixed: HOST = PSP-3000, JOIN = PSP-1000 (the 1000 is the console
whose memory budget matters, so it is the one that RECEIVES the 8 MiB cart).

Arms (each a stage directory hw_loop mirrors onto both cards, host-/join-
prefixed files going to one card only):
  A  Gen 1 trade, Red <-> Red, delay 4.  Scored by the exact trade oracle
     and the one-process reference (make_references.sh).  The fixtures must
     be made on the same cartridge: a Gen 1 save holds ROM pointers, and
     hw1's Blue saves in Red never reached the Cable Club.
  B  Gen 2 trade, Crystal <-> Crystal, delay 4.  Exact trade oracle and the
     reference: the harness freezes the cartridge clock at the seed on both
     consoles (hw1: the PSP-1000's 2011 clock made Crystal stop at "The
     clock's time may be wrong."), so the saves are reproducible.
  C  Gen 1 link BATTLE (Colosseum), Red <-> Red: FIGHT + first move every
     turn until one side faints (~1000 serial exchanges, the lockstep's
     hardest case).  Scored by both scripts reaching battle_start and
     battle_end, every hash matched, and the reference.
  D  B with scanline batching (gblink_batch = 154): while neither machine's
     serial port is busy, slot 0 runs up to a frame ahead instead of the two
     cores alternating every scanline.  Must give B's saves byte for byte
     (proven off-hardware: cable tests at 9 start points x 5 cases, trades
     and battle at delays 0-12 identical to strict alternation); the logs
     show what it saves per frame (core_prof) on the PSP.
  E  C with the same batching.
  V  LIVE link (the owner's no-restart request): both games start RUNNING,
     from save states standing at the Cable Trade Center receptionist; the
     states cross the radio, trade, the host disconnects (the script's
     `disconnect`), both games play on from where they stand.  Scored by the
     trade, both scripts reaching `played_on` still in the Trade Center, the
     saves AND the final frame:hash equal to the session-stack reference.
  U  V without the redundant input datagrams (gblink_input_copies = 0): the
     stutter A/B -- stalls and their longest run, V against U.
  R  V on REAL clocks (no seed; the HOST's clock reads 2011, as when the
     PSP-1000 hosts), then each game boots from its battery save and must
     walk (no "The clock's time may be wrong" -- the owner's report).
  Q  the same with the power-on link the owner's GBLplay used.
  L  B with the radio made worse on purpose: net_latency_ms 30 +
     net_jitter_ms 15 on each side.  Must give B's result exactly, slower.
  X  Transfer, no trade: host plays Crystal, join plays Red, each receives the
     other's cartridge over the radio (gblink_library = 0 forces it although
     both cards hold both games) on the bulk lane at its defaults: 1037-byte
     datagrams (1024 data), adaptive rate.  Times 2 MiB and 1 MiB.
  W  X with 1472-byte datagrams (the largest PDP datagram): is the PSP
     radio's cost per datagram (W faster) or per byte (the same)?
  Y  Transfer: host plays the synthetic 8 MiB cart, join (the PSP-1000)
     receives it -- the 8 MiB memory budget on the smallest console.
  N  Bring-up only (hw5, the NO_PEER stress): Crystal on both, no trade, a
     few seconds linked; both radios start together.  Every run of every arm
     is a relaunch, and each one must find its partner.
  J  N with the HOST's radio 1200 frames (~20 s, the worst eject skew in
     hw4's loop.log) late: the join scans before the host's group exists and
     must keep scanning (a 90 s budget), never create one.
  H  N with the JOIN's radio 600 frames late: the host waits in its group.
  I  the I-cache geometry probe (icache_probe = 1) on both consoles, no game
     (docs/CACHE-MAP.md); score_rig runs icprobe_geometry.py on each log.

Batching (gblink_batch) is explicit in every arm: B is the strict control
(1), everything else runs the player default (154, hw5).
Every run ends DONE on both consoles or it is a failure; hw_loop restores the
golden saves before each run.

Outputs <out>/stage-<arm> for every arm below, <out>/golden, <out>/cards/roms/synth8m.gbc,
<out>/scripts (the staged scripts, for the scorer's I5), and MANIFEST.txt
(md5 of every file, the EBOOT's CRC32 for I3).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TITLE = 'GBLINK rig'
GROUP = 'GBLNK7'
SEED = 1790500000          # the RTC seed both machines start from
DELAY = 4

ROMS = {'red': 'red.gb', 'crystal': 'crystal.gbc', 'synth': 'synth8m.gbc'}
SAVES = {'red': 'red.gb.sav', 'crystal': 'crystal.gbc.sav',
         'synth': 'synth8m.gbc.sav'}
PLAY = 'wait 600\nevt played\n'   # transfer arms: a few seconds, no trade
BATCH_PLAYER = 154         # main_psp.c GBLINK_BATCH_DEFAULT (hw5 default on)
BATCH_STRICT = 1

# arm: (host game, join game, host script, join script, extra ini, library)
ARMS = {
    'A': ('red', 'red', 'gen1_host.ap', 'gen1_guest.ap', {}, 1),
    # B: the strict-alternation control (hw2-hw4's B, unchanged).
    'B': ('crystal', 'crystal', 'gen2_host.ap', 'gen2_guest.ap',
          {'gblink_batch': BATCH_STRICT}, 1),
    'C': ('red', 'red', 'gen1_battle_host.ap', 'gen1_battle_guest.ap', {}, 1),
    'D': ('crystal', 'crystal', 'gen2_host.ap', 'gen2_guest.ap',
          {'gblink_batch': 154}, 1),
    'E': ('red', 'red', 'gen1_battle_host.ap', 'gen1_battle_guest.ap',
          {'gblink_batch': 154}, 1),
    # LIVE link: both games running (the gen2-*.st states, standing at the
    # receptionist), trade, the host disconnects, both play on.
    'V': ('crystal', 'crystal', 'gen2_live_host.ap', 'gen2_live_guest.ap',
          {'live_state': 1, 'gblink_after': 1500, 'gblink_input_copies': 8},
          1),
    # V without the redundant input datagrams: the stutter A/B on the radio.
    'U': ('crystal', 'crystal', 'gen2_live_host.ap', 'gen2_live_guest.ap',
          {'live_state': 1, 'gblink_after': 1500, 'gblink_input_copies': 0}, 1),
    # LIVE link on REAL clocks (no seed; the join's clock set to 2011, as
    # the PSP-1000's reads), then each game boots from its battery save and
    # must reach the overworld without asking for the time.
    # The owner's first hand-played case: the power-on link (GBLplay then),
    # real clocks, the HOST's clock at 2011 (the PSP-1000's): the session
    # clock is the host's, so the join's cartridge clock was stamped 2011 and
    # its next load jumped 15.6 years -- day counter overflow, "The clock's
    # time may be wrong" (the owner's F: save: day 63, carry set, exactly
    # 2026 - 2011 mod 512 days).  Then the battery save must boot clean.
    'Q': ('crystal', 'crystal', 'gen2_powerclock_host.ap',
          'gen2_powerclock_guest.ap',
          {'gblink_live': 0, 'gblink_after': 3000, 'no_seed': 1,
           'gblink_resume': 2, 'host': {'psp_clock': 1298863731}}, 1),
    'R': ('crystal', 'crystal', 'gen2_reload_host.ap', 'gen2_reload_guest.ap',
          {'live_state': 1, 'gblink_after': 3000, 'no_seed': 1,
           'gblink_resume': 2, 'host': {'psp_clock': 1298863731}}, 1),
    'L': ('crystal', 'crystal', 'gen2_host.ap', 'gen2_guest.ap',
          {'net_latency_ms': 30, 'net_jitter_ms': 15}, 1),
    # hw3 timing arms, all B's trade (scored as B: the saves).
    # T: B with the telemetry reporting it can shed shed -- one report at
    # teardown instead of every 600 frames, no stall watcher -- so T vs B is
    # the rig's own cost on the frame loop.
    'T': ('crystal', 'crystal', 'gen2_host.ap', 'gen2_guest.ap',
          {'telemetry_period': 1000000, 'stall_watch_s': 0}, 1),
    # P: B stepping on slot 0's frame boundary on BOTH consoles
    # (gblink_pace_slot0): the join stops needing the host's input a frame
    # before the host needs the join's.
    'P': ('crystal', 'crystal', 'gen2_host.ap', 'gen2_guest.ap',
          {'gblink_pace_slot0': 1}, 1),
    # K: the proposed player defaults together -- P plus scanline batching.
    'K': ('crystal', 'crystal', 'gen2_host.ap', 'gen2_guest.ap',
          {'gblink_pace_slot0': 1, 'gblink_batch': 154}, 1),
    'X': ('crystal', 'red', 'play.ap', 'play.ap', {}, 0),
    'W': ('crystal', 'red', 'play.ap', 'play.ap', {'gblink_bulk_bytes': 1472}, 0),
    'Y': ('synth', 'red', 'play.ap', 'play.ap', {}, 0),
    # hw5 bring-up stress: no trade, no transfer, a short session.
    'N': ('crystal', 'crystal', 'play.ap', 'play.ap', {}, 1),
    'J': ('crystal', 'crystal', 'play.ap', 'play.ap',
          {'host': {'gblink_start_frames': 1200}}, 1),
    'H': ('crystal', 'crystal', 'play.ap', 'play.ap',
          {'join': {'gblink_start_frames': 600}}, 1),
    # I: the I-cache geometry probe on BOTH consoles (psp/icprobe.c): no
    # game, no link; scored by tools/cachemap/icprobe_geometry.py.
    'I': ('crystal', 'crystal', 'play.ap', 'play.ap',
          {'host': {'icache_probe': 1}, 'join': {'icache_probe': 1}}, 1),
}


def md5(p):
    h = hashlib.md5()
    with open(p, 'rb') as fh:
        for b in iter(lambda: fh.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def crc32(p):
    with open(p, 'rb') as fh:
        return '%08x' % (zlib.crc32(fh.read()) & 0xFFFFFFFF)


def ini(role, game, extra, library, arm):
    per_role = extra.get(role, {})
    lines = [
        '# GB link rig (prepare_gblink_rig.py) arm %s -- run_id and arm are '
        'appended by hw_loop' % arm,
        'rom = %s' % ROMS[game],
        'script = link.ap',
        '%s = 1' % role,
        'nick = %s' % role,
        'group = %s' % GROUP,
        'gblink_delay = %d' % DELAY,
    ] + ([] if extra.get('no_seed') else ['gblink_rtc_seed = %d' % SEED]) + [
        'load_state = %d' % (1 if extra.get('live_state') else 0),
        'gblink_after = %d' % extra.get('gblink_after', 0),
        'gblink_resume = %d' % extra.get('gblink_resume', 0),
        'gblink_input_copies = %d' % extra.get('gblink_input_copies', 0),
        'gblink_pace_slot0 = %d' % extra.get('gblink_pace_slot0', 0),
        'telemetry_period = %d' % extra.get('telemetry_period', 0),
        'gblink_live = %d' % extra.get('gblink_live', 1),
        'gblink_end_after = 120',
        'gblink_library = %d' % library,
        'gblink_bulk_bytes = %d' % extra.get('gblink_bulk_bytes', 1037),
        'gblink_batch = %d' % extra.get('gblink_batch', BATCH_PLAYER),
        'net_latency_ms = %d' % extra.get('net_latency_ms', 0),
        'net_jitter_ms = %d' % extra.get('net_jitter_ms', 0),
        'log_input = 0',
        # A safety net only: the longest arm ends near 12,000 frames.
        'autoexit_frames = 36000',
        'handoff = 1',
        'handoff_window_s = 90',
        'handoff_max_runs = 100000',
        'handoff_total_s = 900',
        'handoff_park_s = 0',
        'stall_watch_s = %d' % extra.get('stall_watch_s', 20),
    ] + ['%s = %s' % kv for kv in sorted(per_role.items())]
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--fix', required=True)
    ap.add_argument('--backup', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--release', help='the release build (EBOOT.PBP + prx) '
                    'for the hand-played GBADHOC-GBLINK-PLAY folder')
    ap.add_argument('--m1',help='folder holding the milestone-1 '
                    'GBADHOC-GBLINK-{DUAL,SOLO,GBA} (ini + states); they are '
                    'copied onto this session binary')
    ap.add_argument('--allow-dirty', action='store_true',
                    help='accept a build whose build-manifest.json says the '
                         'working tree was dirty (never for a hardware rig)')
    ap.add_argument('--reference', required=True,
                    help='make_references.sh output (gen1/gen2/battle-d4-*.sav '
                         'and gen1-rom.sha1) from the same --fix')
    a = ap.parse_args()

    # A rig binary must be reproducible from its commit (hw4's and the first
    # hw5 draft's were flagged dirty by untracked __pycache__).
    import json
    builds = {}
    for what, d in (('build', a.build), ('release', a.release)):
        if not d:
            continue
        mp = os.path.join(d, 'build-manifest.json')
        if not os.path.isfile(mp):
            sys.exit('REFUSING: %s has no build-manifest.json (tools/build.sh '
                     '--out writes one)' % d)
        with open(mp) as fh:
            bm = json.load(fh)
        if bm.get('workingTreeClean') is not True and not a.allow_dirty:
            sys.exit('REFUSING: the %s build (%s) was made from a dirty tree '
                     '(workingTreeClean=%s)' % (what, d, bm.get('workingTreeClean')))
        builds[what] = bm
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        sys.exit('REFUSING: %s exists; remove it first' % out)
    os.makedirs(out)
    # One binary for the whole session, relabelled so it cannot be mistaken
    # for the player build on the XMB.
    sys.path.insert(0, os.path.join(REPO, 'tools'))
    from relabel_pbp import relabel  # noqa: E402
    build = os.path.join(out, 'build')
    os.makedirs(build)
    with open(os.path.join(a.build, 'EBOOT.PBP'), 'rb') as fh:
        pbp_raw = fh.read()
    pbp = relabel(pbp_raw, TITLE)
    with open(os.path.join(build, 'EBOOT.PBP'), 'wb') as fh:
        fh.write(pbp)
    shutil.copy2(os.path.join(a.build, 'gbadhoc_me.prx'), build)
    for extra in ('build-manifest.json',):
        if os.path.isfile(os.path.join(a.build, extra)):
            shutil.copy2(os.path.join(a.build, extra), build)

    scripts = os.path.join(out, 'scripts')
    os.makedirs(scripts)
    for f in ('gen1_host.ap', 'gen1_guest.ap', 'gen2_host.ap', 'gen2_guest.ap',
              'gen1_battle_host.ap', 'gen1_battle_guest.ap',
              'gen2_live_host.ap', 'gen2_live_guest.ap',
              'gen2_reload_host.ap', 'gen2_reload_guest.ap',
              'gen2_powerclock_host.ap', 'gen2_powerclock_guest.ap'):
        shutil.copy2(os.path.join(HERE, 'scripts', f), scripts)
    with open(os.path.join(scripts, 'play.ap'), 'w', newline='\n') as fh:
        fh.write(PLAY)

    cfg = {'host': os.path.join(a.backup, 'F-PSP-3000', 'GBADHOC', 'CONFIG.INI'),
           'join': os.path.join(a.backup, 'D-PSP-1000', 'GBADHOC', 'CONFIG.INI')}
    for role, p in cfg.items():
        if not os.path.isfile(p):
            sys.exit('REFUSING: no owner CONFIG.INI for %s at %s' % (role, p))

    for arm, (hg, jg, hs, js, extra, lib) in ARMS.items():
        st = os.path.join(out, 'stage-' + arm)
        os.makedirs(st)
        shutil.copy2(os.path.join(build, 'EBOOT.PBP'), st)
        shutil.copy2(os.path.join(build, 'gbadhoc_me.prx'), st)
        for role, game, script in (('host', hg, hs), ('join', jg, js)):
            with open(os.path.join(st, role + '-.gpsp-harness.ini'), 'w',
                      newline='\n') as fh:
                fh.write(ini(role, game, extra, lib, arm))
            shutil.copy2(os.path.join(scripts, script),
                         os.path.join(st, role + '-link.ap'))
            # The owner's own settings, so the rig runs what he plays with.
            shutil.copy2(cfg[role], os.path.join(st, role + '-CONFIG.INI'))

    gold = os.path.join(out, 'golden')
    os.makedirs(gold)
    for src, dst in (('gen1-a.sav', 'host-' + SAVES['red']),
                     ('gen1-b.sav', 'join-' + SAVES['red']),
                     ('gen2-a.sav', 'host-' + SAVES['crystal']),
                     ('gen2-b.sav', 'join-' + SAVES['crystal']),
                     # the live-link games (hw_loop restores them each run;
                     # only arms with load_state = 1 read them)
                     ('gen2-a.st', 'host-crystal.gbc.st0'),
                     ('gen2-b.st', 'join-crystal.gbc.st0')):
        shutil.copy2(os.path.join(a.fix, src), os.path.join(gold, dst))
    with open(os.path.join(gold, 'host-' + SAVES['synth']), 'wb') as fh:
        fh.write(bytes(32768))
    # The one-process results, and the Gen 1 cartridge the fixtures and
    # references belong to (setup_gblink_cards.py refuses any other).
    sha = open(os.path.join(a.fix, 'gen1-rom.sha1')).read().strip()
    if open(os.path.join(a.reference, 'gen1-rom.sha1')).read().strip() != sha:
        sys.exit('REFUSING: --reference was made from another Gen 1 cartridge')
    ref = os.path.join(out, 'reference')
    os.makedirs(ref)
    for f in ('gen1-d4-a.sav', 'gen1-d4-b.sav', 'gen2-d4-a.sav',
              'gen2-d4-b.sav', 'battle-d4-a.sav', 'battle-d4-b.sav',
              'live-d4-a.sav', 'live-d4-b.sav', 'live-d4.final',
              'gen1-rom.sha1'):
        shutil.copy2(os.path.join(a.reference, f), ref)

    cards = os.path.join(out, 'cards', 'roms')
    os.makedirs(cards)
    subprocess.run([sys.executable, os.path.join(HERE, 'mksynth.py'),
                    os.path.join(cards, ROMS['synth']), str(8 << 20)], check=True)

    if a.release:
        # The player build, for the owner to try the menus by hand
        # (GBADHOC-GBLINK-PLAY: no harness file, so it boots to the browser).
        play = os.path.join(out, 'play')
        os.makedirs(play)
        with open(os.path.join(a.release, 'EBOOT.PBP'), 'rb') as fh:
            rel = relabel(fh.read(), 'GBLplay')   # the release SFO holds 8 bytes
        with open(os.path.join(play, 'EBOOT.PBP'), 'wb') as fh:
            fh.write(rel)
        shutil.copy2(os.path.join(a.release, 'gbadhoc_me.prx'), play)
        if os.path.isfile(os.path.join(a.release, 'build-manifest.json')):
            shutil.copy2(os.path.join(a.release, 'build-manifest.json'), play)

    if a.m1:
        # Milestone 1's three measurement folders (harness ini + save
        # states), on THIS session's binary, each under its own XMB title.
        # DUAL8 / DUAL154: the same pair with each core running 8 / 154
        # scanlines (a whole frame) before the other -- what the switching
        # between two copies of the core's code costs in the instruction
        # cache (bench only: link sessions always switch every line).
        # SOLOH: the partner machine alone -- its save state (partner.st),
        # headless -- what the second Game Boy costs without the switching.
        for sub, title, src, batch in (
                ('GBADHOC-GBLINK-DUAL', 'GBLINK dual', 'GBADHOC-GBLINK-DUAL', 0),
                ('GBADHOC-GBLINK-DUAL8', 'GBLINK dual8', 'GBADHOC-GBLINK-DUAL', 8),
                ('GBADHOC-GBLINK-DUALF', 'GBLINK dualF', 'GBADHOC-GBLINK-DUAL', 154),
                ('GBADHOC-GBLINK-SOLO', 'GBLINK solo', 'GBADHOC-GBLINK-SOLO', 0),
                ('GBADHOC-GBLINK-SOLOH', 'GBLINK soloH', 'GBADHOC-GBLINK-SOLO', -1),
                ('GBADHOC-GBLINK-GBA', 'GBLINK GBA', 'GBADHOC-GBLINK-GBA', 0)):
            dst = os.path.join(out, 'm1', sub)
            shutil.copytree(os.path.join(a.m1, src), dst)
            if batch > 0:
                with open(os.path.join(dst, '.gpsp-harness.ini'), 'a',
                          newline='\n') as fh:
                    fh.write('\ngb_dual_batch = %d\n' % batch)
            if batch < 0:
                shutil.copy2(os.path.join(a.m1, 'GBADHOC-GBLINK-DUAL', 'roms',
                                          'partner.st'),
                             os.path.join(dst, 'roms', 'crystal.gbc.st0'))
                with open(os.path.join(dst, '.gpsp-harness.ini'), 'a',
                          newline='\n') as fh:
                    fh.write('\ngb_headless = 1\n')
            with open(os.path.join(dst, 'EBOOT.PBP'), 'wb') as fh:
                fh.write(relabel(pbp_raw, title))
            shutil.copy2(os.path.join(build, 'gbadhoc_me.prx'), dst)

    rows = []
    for root, _dirs, files in os.walk(out):
        for f in sorted(files):
            p = os.path.join(root, f)
            rows.append('%s  %s' % (md5(p), os.path.relpath(p, out)))
    with open(os.path.join(out, 'MANIFEST.txt'), 'w', newline='\n') as fh:
        fh.write('# prepare_gblink_rig.py -- EBOOT crc32 %s (the scorer\'s '
                 '--expect-crc)\n' % crc32(os.path.join(build, 'EBOOT.PBP')))
        for what, bm in sorted(builds.items()):
            fh.write('# %s build: commit %s clean=%s profile=%s eboot md5 %s\n'
                     % (what, bm.get('sourceCommit'), bm.get('workingTreeClean'),
                        bm.get('profile'), (bm.get('eboot') or {}).get('md5')))
        if a.release:
            fh.write('# player build (play/EBOOT.PBP, relabelled) crc32 %s\n'
                     % crc32(os.path.join(out, 'play', 'EBOOT.PBP')))
        fh.write('\n'.join(sorted(rows)) + '\n')
    print('rig staged in %s; EBOOT crc32 %s' % (out, crc32(os.path.join(build, 'EBOOT.PBP'))))


if __name__ == '__main__':
    main()
