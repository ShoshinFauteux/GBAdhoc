#!/usr/bin/env python3
"""Single PSP performance queue. Requires the window-aware USB handoff build.

Only operates inside the campaign's marked GBADHOC-PERF directory. Queue jobs
are immutable directories with job.json and SHA256-pinned payloads. Publishing
a new queue.json is enough to resume a parked rig. The first launch is manual.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import time


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def atomic(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.pending')
    with temp.open('wb') as out:
        out.write(data)
        out.flush()
        os.fsync(out.fileno())
    os.replace(temp, path)


def save_json(path, data):
    atomic(path, (json.dumps(data, indent=2) + '\n').encode())


def fields(text):
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)


def events(text, name):
    return [dict(re.findall(r'(\w+)=([^\s]+)', line))
            for line in text.splitlines() if line.startswith('EVT ' + name + ' ')]


def score(text, job, result, require_me=None):
    """Full windows only; diagnostics retained but never reported as scores.

    TWO CHECKS ARE JOB-DECLARED, because a renderer A/B cannot satisfy the
    defaults and no caller ever overrode them:

      require_me   the me0 arm of a me_mode A/B is DEFINED by the engine being
                   off, so a hardcoded require_me=True made that arm permanently
                   invalid -- and an invalid run halts the collector, so the
                   experiment could never get past its own control arm.
      vhash_only   such a job's product is the EVT vhash stream, not a score.
                   The EVT ring drops lines under load, which costs a
                   perf_window and fails the window reconstruction; fatal for a
                   measurement, irrelevant to a hash comparison.

    Both DEFAULT TO THE STRICT BEHAVIOUR.  A perf job that does not name them is
    validated exactly as before, which is the point: this widens what can be
    asked for without weakening what is already asked.
    """
    if require_me is None:
        require_me = job.get('require_me', True)
    vhash_only = job.get('vhash_only', False)
    errors = []
    if result.get('exit') != '0': errors.append('nonzero exit')
    if result.get('reason', 'ok') != 'ok': errors.append('abnormal exit reason')
    starts = events(text, 'perf_job')
    if len(starts) != 1 or starts[0].get('id') != job['id'] or starts[0].get('fixture') != job['fixture']:
        errors.append('job identity mismatch')
    if not events(text, 'autoload_state') or any(e.get('rc') != '0' for e in events(text, 'autoload_state')):
        errors.append('state load failed or absent')
    if not events(text, 'ap_done') or events(text, 'ap_fail'): errors.append('script incomplete')
    if len(events(text, 'frame_dump')) < 2: errors.append('scene snapshots missing')
    expected = job['to'] - job['from']
    if events(text, 'perf_done') != [{'samples': str(expected), 'expected': str(expected)}]:
        errors.append('incomplete sample range')
    windows = events(text, 'perf_window')
    hist = [0] * 8
    n = wall = work = over = worst = 0
    last = job['from']
    try:
        for w in windows:
            count, frame = int(w['n']), int(w['f'])
            bins = [int(x) for x in w['hist'].split(',')]
            if count != min(300, job['to'] - last) or frame != last + count or len(bins) != 8 or sum(bins) != count:
                raise ValueError('window discontinuity')
            if int(w['wall_us']) <= 0 or not 0 <= int(w['over']) <= count or min(bins) < 0:
                raise ValueError('invalid counters')
            n += count
            wall += int(w['wall_us'])
            work += int(w['work_us'])
            over += int(w['over'])
            worst = max(worst, int(w['wall_max']))
            hist = [a+b for a,b in zip(hist,bins)]
            last = frame
        if n != expected or last != job['to']: raise ValueError('missing windows')
    except (KeyError, ValueError):
        # A vhash_only job still accumulates whatever windows arrived, for
        # context, but a gap in them is not a failure of what it measures.
        # perf_done above still proves the run covered its whole range.
        if not vhash_only:
            errors.append('invalid measurement windows')
    if require_me and 'EVT me_rend on ' not in text: errors.append('ME renderer not enabled')
    if require_me and any(e.get('reason') != 'exit' for e in events(text, 'me_rend off')):
        errors.append('ME renderer fell back')
    if job['core_phase'] and not any(e.get('lvl') == str(job['core_phase']) for e in events(text, 'core_phase')):
        errors.append('requested profiler absent')
    if job.get('audio_oracle'):
        audio = events(text, 'audio_hash')
        if len(audio) != 1 or int(audio[0].get('samples', '0')) <= 0:
            errors.append('audio oracle absent')
    return {'id': job['id'], 'fixture': job['fixture'], 'valid': not errors,
            # vhash_only never scores: its aggregates may come from a truncated
            # window set and must never be quoted as performance.
            'scoring': not errors and job['core_phase'] == 0
                       and job.get('scoring', True) and not vhash_only,
            'errors': errors,
            'samples': n, 'wall_us': wall, 'work_us': work,
            'emu_fps': n*1e6/wall if wall else None,
            'work_mean_us': work/n if n else None,
            'over_budget_frames': over, 'worst_wall_us': worst, 'wall_histogram': hist}


def safe_name(name):
    p = PurePosixPath(name)
    if '\\' in name or ':' in name or p.is_absolute() or '..' in p.parts or str(p) != name:
        raise ValueError('unsafe package path: ' + name)
    return name


def load_job(directory):
    directory = Path(directory)
    job = read_json(directory/'job.json')
    if not re.fullmatch(r'[a-zA-Z0-9_-]{1,48}', job['id']): raise ValueError('invalid job id')
    required = {'EBOOT.PBP', 'gbadhoc_me.prx', 'CONFIG.INI', '.gpsp-harness.ini', 'battle.inputs'}
    if not required.issubset(job['files']): raise ValueError('incomplete package')
    # THE EBOOT MUST BE A HARNESS BUILD.  A release EBOOT is a valid PBP of a
    # plausible size, so presence and size checks pass happily -- but ADR-0067
    # makes it unable to read .gpsp-harness.ini, so it boots to the ROM browser,
    # never auto-loads, and never opens a handoff window.  The console then looks
    # broken and the collector waits forever.  This has happened: a release build
    # left in psp/EBOOT.PBP by an earlier `tools/build.sh release` was copied into
    # four job payloads, and the only visible symptom was the XMB title reading
    # "GBAdhoc" instead of "GBAdhoc HARNESS".
    #
    # The discriminator is the ini path string the binary carries, which is the
    # same thing tools/build.sh audits per profile.
    eboot = (directory/'EBOOT.PBP').read_bytes()
    if b'.playable-no-harness' in eboot:
        raise ValueError('EBOOT.PBP is a RELEASE build (carries '
                         '.playable-no-harness): it cannot read the harness ini, '
                         'so it will boot to the ROM browser and never hand off. '
                         'Rebuild with tools/build.sh harness and re-stage.')
    # Deliberately asymmetric: only a POSITIVE release marker is rejected.
    # Requiring the harness marker too would demand every test fixture be a
    # realistic PBP, and it is the weaker half of the check anyway -- a release
    # build is what needs catching, and it identifies itself.
    for name, digest in job['files'].items():
        safe_name(name)
        if name not in required and not (name.startswith('roms/') and Path(name).suffix in ('.st0', '.sav')):
            raise ValueError('unexpected payload: ' + name)
        if sha(directory/name) != digest: raise ValueError('package hash mismatch: ' + name)
    for name in job['resident']:
        safe_name(name)
    if not 0 <= job['from'] < job['to'] <= 36000 or job['core_phase'] not in (0, 2, 3):
        raise ValueError('invalid measurement settings')
    return job


def check_device(root, marker):
    root = Path(root).resolve()
    if root.name != 'GBADHOC-PERF' or root.parent.name != 'GAME' or root.parent.parent.name != 'PSP':
        raise ValueError('not the dedicated rig directory')
    if (root/'RIG.ID').read_text().strip() != marker: raise ValueError('wrong device marker')


def ps(script):
    subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command',
                    "$ErrorActionPreference='Stop'; " + script], check=True,
                   capture_output=True, timeout=15)


def flush(root):
    drive = Path(root).drive
    if not re.fullmatch('[A-Za-z]:', drive): raise ValueError('not a Windows drive')
    ps('Write-VolumeCache -DriveLetter ' + drive[0])


def stage(directory, root, marker, deadline, flush_fn=flush):
    """Caller archives the previous result first. RUN is the last publication."""
    job = load_job(directory)
    root, directory = Path(root), Path(directory)
    check_device(root, marker)
    result_path = root/'handoff/RESULT.TXT'
    result = result_path.read_bytes()
    if fields(result.decode()).get('status') not in ('ready', 'parked'):
        raise ValueError('not a handoff')
    if (root/'handoff/CMD.TXT').exists(): raise ValueError('command already pending')
    for name, digest in job['resident'].items():
        if sha(root/name) != digest: raise ValueError('resident ROM/BIOS mismatch: ' + name)
    if time.monotonic() + 40 >= deadline: return False
    for name, digest in job['files'].items():
        if time.monotonic() + 30 >= deadline: raise TimeoutError('staging window exhausted')
        atomic(root/name, (directory/name).read_bytes())
        if sha(root/name) != digest: raise IOError('staged hash mismatch: ' + name)
    save_json(root/'JOB.JSON', job)
    flush_fn(root)  # All payloads durable before a RUN command can exist.
    # USB mass-storage caches can acknowledge the per-file readback above and
    # still commit different bytes when the volume cache is flushed.  Verify
    # the durable view before publishing RUN; a corrupt control file must park
    # the rig rather than launch an uninstrumented game.
    for name, digest in job['files'].items():
        if sha(root/name) != digest:
            raise IOError('post-flush staged hash mismatch: ' + name)
    if read_json(root/'JOB.JSON') != job:
        raise IOError('post-flush job manifest mismatch')
    if time.monotonic() + 20 >= deadline or result_path.read_bytes() != result:
        raise TimeoutError('handoff changed before commit')
    cmd = root/'handoff/CMD.TXT'
    atomic(cmd, b'RUN\n')
    try:
        flush_fn(root)
    except Exception:
        # Payload is complete, but command durability is uncertain. Stop host
        # automation and best-effort withdraw it; never continue to another job.
        cmd.unlink(missing_ok=True)
        flush_fn(root)
        raise
    return True


def collect(root, dest, job, result):
    dest.mkdir(parents=True, exist_ok=True)
    names = ['JOB.JSON', 'handoff/RESULT.TXT', 'handoff/WINDOW.TXT', 'handoff/STATE.TXT']
    names += [p.relative_to(root).as_posix() for p in (root/'log').rglob('*') if p.is_file()]
    names += [name for name in job['files'] if name.startswith('roms/')]
    for name in names:
        src, dst = root/name, dest/name
        if src.is_file():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
            if sha(src) != sha(dst): raise IOError('archive mismatch: ' + name)
    text = (dest/'log/frontend.log').read_text(errors='replace')
    summary = score(text, job, result)
    # Saves can legitimately change at shutdown. Executable/config/state must
    # still match the published job, including after a collector restart.
    for name, digest in job['files'].items():
        if not name.endswith('.sav') and sha(root/name) != digest:
            summary['errors'].append('post-run hash mismatch: ' + name)
            summary['valid'] = summary['scoring'] = False
    save_json(dest/'summary.json', summary)
    return summary


def run(campaign):
    base = Path(campaign).resolve()
    config = read_json(base/'campaign.json')
    root = Path(config['device'])
    journal_path = base/'journal.json'
    journal = read_json(journal_path) if journal_path.exists() else {'completed': [], 'dispatched': []}
    previous_token = None
    first_poll = True
    last_seen = time.monotonic()
    print('Waiting for manual first launch and a fresh USB window.', flush=True)
    while not (base/'STOP').exists():
        if time.monotonic() - last_seen > config.get('timeout_s', 1800):
            raise TimeoutError('PSP unavailable; no further staging')
        try:
            window = fields((root/'handoff/WINDOW.TXT').read_text())
            token = window['token']
        except (OSError, KeyError):
            first_poll = False
            time.sleep(.5)
            continue
        if first_poll:
            # A collector started mid-window must wait for the NEXT token.
            previous_token, first_poll = token, False
        if token == previous_token:
            time.sleep(.5)
            continue
        previous_token = token
        observed = last_seen = time.monotonic()
        deadline = observed + min(int(window['seconds']), 300) - 15
        check_device(root, config['marker'])
        result = fields((root/'handoff/RESULT.TXT').read_text())
        if result.get('status') not in ('ready', 'parked'): raise ValueError('malformed result')
        job = read_json(root/'JOB.JSON')
        key = result['run'] + '-' + job['id']
        if key not in journal['completed']:
            # A staged-but-not-launched job still has the previous RESULT.
            awaiting = journal.get('awaiting')
            if awaiting and int(result['run']) <= awaiting['after_run']:
                continue
            if (root/'handoff/CMD.TXT').exists():
                continue
            try:
                summary = collect(root, base/'runs'/key, job, result)
            except OSError as error:
                # A PSP handoff can disappear between copying the run and the
                # final device hash read.  Keep the journal uncommitted and
                # retry this same result on the next fresh USB token instead
                # of killing the collector or advancing the queue.
                print('Collect deferred after removable-drive I/O error: ' +
                      str(error), flush=True)
                continue
            print(json.dumps(summary), flush=True)
            if not summary['valid']:
                save_json(base/'HALTED.json', summary)
                raise RuntimeError('invalid run; rig left parked')
            journal['completed'].append(key)
            save_json(journal_path, journal)
        queue = read_json(base/'queue.json')
        jobs = [(base/p, load_job(base/p)) for p in queue]
        ids = [j['id'] for _,j in jobs]
        if len(set(ids)) != len(ids): raise ValueError('duplicate queue job')
        finished = {x.split('-', 1)[1] for x in journal['completed']}
        pending = [(p,j) for p,j in jobs if j['id'] not in finished]
        if not pending: continue  # Remain parked; agent can publish more jobs.
        directory, next_job = pending[0]
        if (base/'STOP').exists(): break
        if stage(directory, root, config['marker'], deadline):
            journal['dispatched'].append(next_job['id'])
            # Dispatch belongs to a particular preceding PSP run. The next
            # RESULT's increment proves the new job actually executed.
            journal['awaiting'] = {'id': next_job['id'], 'after_run': int(result['run'])}
            save_json(journal_path, journal)
            print('Staged and flushed ' + next_job['id'], flush=True)
            # HAND THE CARD BACK, THEN CHECK THAT IT WENT.
            #
            # InvokeVerb("Eject") reports failure by doing nothing: no exception,
            # no status.  The except arm below only catches PowerShell failing to
            # launch.  The eject itself fails whenever another process holds a
            # handle on the drive -- an `ls` or `cat` in the handoff directory is
            # enough -- and then the console never gets its card back, never reads
            # CMD.TXT, and waits out its window.
            #
            # From the host that looks exactly like a dead console: the token
            # stops advancing and CMD.TXT stays put.  It has been misread that
            # way, at the cost of two needless app launches.  So say it plainly.
            #
            # Deliberately NOT retried: an eject blocked by a held handle will not
            # succeed on a second call, and the payload and RUN are already
            # flushed, so the next window picks the job up once the handle goes.
            ejected = False
            try:
                ps('(New-Object -ComObject Shell.Application).Namespace(17)'
                   '.ParseName("' + root.drive + '").InvokeVerb("Eject")')
                for _ in range(3):           # the drive going away IS the proof
                    if not root.exists():
                        ejected = True
                        break
                    time.sleep(.5)
            except subprocess.SubprocessError:
                print('Eject call failed to launch; payload and RUN were '
                      'flushed. Waiting for a fixed window.', flush=True)
            else:
                if not ejected:
                    print('EJECT DID NOT RELEASE %s -- something is holding a '
                          'handle on it. The console cannot read CMD.TXT until '
                          'it is released, so its window will expire and its '
                          'token will stop advancing. THAT IS NOT A DEAD '
                          'CONSOLE. Payload and RUN are flushed; the next '
                          'window will pick the job up.' % root.drive,
                          flush=True)
        time.sleep(.5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--campaign', required=True)
    args = parser.parse_args()
    run(args.campaign)
