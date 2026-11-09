#!/usr/bin/env python3
"""pp_sweep.py -- G0var, the variant -E sweep (PLAN-P-beta5 §4 common), behind pp_sweep.sh.

    pp_sweep.sh --ref TREE_A --new TREE_B --logs RUN_DIR [--overlay DIR] [--keep DIR]
                [--allow-prune NAME]... [--arms "A B ..."] [--flavours "..."] [--list]
                [--max-order 2] [--sde N] [--image I] [--jobs N]

WHAT IS SWEPT (adversarial review 2026-11-07: the old sweep toggled one hand-listed
switch at a time on the release line only, so a change under MGIFT=0 x RFU_SCHED=0
or REWIND=0 x MGIFT=0 passed; the lab and harness lines were never swept; a rewrite
of `MGIFT="${MGIFT:-1}"` silently pruned its arm).

  * The SWITCHES are read from each tree's own tools/build.sh: every environment
    input `${NAME:-D}` whose value build.sh restricts to digits (`case "$NAME" in
    0|1)`, `case "${NAME:-}" in 0|1|2)`, `[ "${NAME:-D}" = 1 ]`).  Every other input
    must be one of FREEFORM (pins, version, pads, extras), or the sweep refuses
    (exit 2): a new build.sh input cannot be missed silently.  Both trees must have
    the same switch set.
  * The ARMS: the flavour's own configuration (`base`, i.e. G0), every switch value
    alone, and every pair of values of two different switches (--max-order 3 adds
    triples), on EVERY flavour: release, release-stable, lab, harness.
  * The COMPILE LINES are not typed: for each (flavour, arm) the tree's build.sh is
    run dry (under `env -i`, with a fake `docker` that records the psp make command
    and stops there), and `make -n -B` with that command in the real psp/Makefile
    gives every TU's compile line.  The derived base line of each flavour must equal
    the line in RUN_DIR/<flavour>/logs/.build-eboot.log (proof the derivation is the
    build; a log from a leaky environment, e.g. ME_LOWLAT=0 exported, fails here).
  * The FILES: psp/main_psp.c, every TU the step changes, and every TU that includes
    (textually or with -include) a header or fragment the step changes.  A changed
    file outside the -E view (a link script, a core TU) is a skip.
  Arms whose derived line equals another's are preprocessed once (same input, same
  stream): that is deduplication, not a prune.

EXIT 0 every arm identical; 1 a difference; 2 a skipped (pruned) arm, flavour or
file, or a tool error; 3 identical but partial (--arms / --flavours chose a subset:
not a gate).  The only escape for a skip is --allow-prune NAME, where NAME is an arm
(e.g. MGIFT=0+RFU_SCHED=0), a flavour, or a file path relative to gpsp/; every
allowed prune is printed in the summary line.
"""
import argparse
import hashlib
import itertools
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pp_norm import norm  # noqa: E402

FOUR = ('release', 'release-stable', 'lab', 'harness')
PROFILE = {'release': 'release', 'release-stable': 'release', 'lab': 'lab', 'harness': 'harness'}
# build.sh inputs that are not switches: identity, pins, pads, extras and budgets.
# They are never swept; flavours.sh records them (buildEnv).
FREEFORM = {'GPSP_BUILD_COMMIT', 'GPSP_BUILD_TREE', 'GPSP_BUILD_DIRTY', 'GPSP_BUILD_VERSION',
            'DOCKER_IMAGE', 'GPSP_EBOOT_TITLE', 'CORE_EXTRA', 'FE_EXTRA', 'PSP_EXTRA',
            'LAYOUT_PAD_BYTES', 'LAYOUT_LOCK', 'BUDGET_HPI_1000', 'BUDGET_HPI_GO',
            'BUDGET_BOOT_MIN', 'IMAGE_BYTES'}
SKIP_EXT = ('.o', '.d', '.a', '.elf', '.PBP', '.prx', '.map', '.su', '.pyc', '.log', '.nm')
IGNORED = re.compile(r'(^|/)(docs/|README|.*\.md$)|^tools/(?!build\.sh$|layout/)')
RECIPE = re.compile(r'^(tools/build\.sh|psp/Makefile|Makefile|tools/layout/.*)$')
LINE_RE = re.compile(r' -c -o (\S+) (\S+)$')


def die(msg, rc=2):
    print('pp_sweep: %s' % msg)
    sys.exit(rc)


# ------------------------------------------------------------------ switches
def switches(build_sh):
    """-> (switches {name: (default, sorted values)}, inputs set)."""
    txt = open(build_sh, encoding='utf-8', errors='replace').read()
    inputs = set(re.findall(r'\$\{([A-Z_][A-Z0-9_]*)(?::?[-+])', txt))
    default = {}
    for n, d in re.findall(r'\$\{([A-Z_][A-Z0-9_]*):-([^}]*)\}', txt):
        default.setdefault(n, d.strip('"\''))
    dom = {}
    lines = txt.split('\n')
    for i, ln in enumerate(lines):
        m = re.search(r'\bcase\s+"?\$\{?([A-Z_][A-Z0-9_]*)(?::-[^}]*)?\}?"?\s+in\b(.*)$', ln)
        if m and m.group(1) in inputs:
            rest = m.group(2)
            alt = re.match(r'\s*([0-9]+(?:\|[0-9]+)*)\)', rest)
            k = i + 1
            while not alt and k < len(lines) and k < i + 4:
                alt = re.match(r'\s*([0-9]+(?:\|[0-9]+)*)\)', lines[k])
                k += 1
            if alt:
                dom.setdefault(m.group(1), set()).update(alt.group(1).split('|'))
        for n, d, op, c in re.findall(r'\[\s+"\$\{([A-Z_][A-Z0-9_]*):-([^}]*)\}"\s+(!?=)\s+"?([0-9]+)"?\s+\]', ln):
            dom.setdefault(n, set()).update({c, d} if d.isdigit() else {c})
            if c in ('0', '1'):
                dom[n].update({'0', '1'})
    sw = {n: (default.get(n, ''), sorted(v)) for n, v in dom.items()}
    unclassified = sorted(inputs - set(sw) - FREEFORM)
    return sw, inputs, unclassified


# ------------------------------------------------------------------ dry build.sh
SHIM = r'''#!/usr/bin/env bash
# pp_sweep dry docker: record the psp make command build.sh would run, stop there
[ "$1" = run ] || exit 0
shift; wd=""; root=""; cmd=""
while [ $# -gt 0 ]; do
  case "$1" in
    -w) wd=$2; shift 2 ;;
    -v) case "$2" in *:/build) root=${2%:/build} ;; esac; shift 2 ;;
    -e) shift 2 ;;
    --rm) shift ;;
    -c) cmd=$2; shift 2 ;;
    *) shift ;;
  esac
done
case "$wd:$cmd" in
  /build:*print-features*) echo "SMC_GATES=1" ;;
  /build:make\ platform=psp1*) : > "$root/gpsp_libretro_psp1.a" ;;
  /build/psp:*print-features*) echo "ND_WINDOW=1" ;;
  /build/psp:make\ *) printf '%s' "$cmd" > "$DRY_REC"; exit 97 ;;
esac
exit 0
'''


class Side:
    def __init__(self, name, tree, overlay, work):
        self.name, self.tree, self.overlay = name, tree, overlay
        self.dry = os.path.join(work, 'dry_' + name)
        for rel in ('tools/build.sh', 'psp/Makefile', 'gpsp_features.h', 'psp/fe_features.h'):
            src = self.path(rel)
            if os.path.exists(src):
                os.makedirs(os.path.dirname(os.path.join(self.dry, rel)), exist_ok=True)
                shutil.copy(src, os.path.join(self.dry, rel))
        lay = self.path('tools/layout')
        if os.path.isdir(lay):
            shutil.copytree(lay, os.path.join(self.dry, 'tools/layout'))
        os.makedirs(os.path.join(self.dry, '.bin'), exist_ok=True)
        with open(os.path.join(self.dry, '.bin/docker'), 'w', newline='\n') as f:
            f.write(SHIM)
        os.chmod(os.path.join(self.dry, '.bin/docker'), 0o755)
        self.recipe = hashlib.sha256(b''.join(
            open(os.path.join(self.dry, r), 'rb').read() for r in ('tools/build.sh', 'psp/Makefile')
            if os.path.exists(os.path.join(self.dry, r)))).hexdigest()

    def path(self, rel):
        if self.overlay and os.path.exists(os.path.join(self.overlay, rel)):
            return os.path.join(self.overlay, rel)
        return os.path.join(self.tree, rel)

    def make_cmd(self, profile, env, tag):
        # each dry run in its own copy: build.sh writes $ROOT/.build-*.log, and a shared
        # root let one run read another's features log
        root = self.dry + '_' + tag
        shutil.rmtree(root, ignore_errors=True)
        shutil.copytree(self.dry, root, symlinks=True)
        rec = os.path.join(root, '.rec')
        e = {'PATH': os.path.join(root, '.bin') + ':/usr/bin:/bin', 'HOME': root,
             'DRY_REC': rec, 'DOCKER_IMAGE': 'dry', 'GIT_DIR': '/nonexistent/pp_sweep-dry'}
        e.update(env)
        r = subprocess.run(['bash', os.path.join(root, 'tools/build.sh'), profile],
                           env=e, cwd=root, capture_output=True, text=True)
        try:
            if not os.path.exists(rec):
                return None, (r.stdout + r.stderr)[-600:]
            return open(rec).read(), None
        finally:
            shutil.rmtree(root, ignore_errors=True)


# ------------------------------------------------------------------ docker
def docker_batch(image, tree, overlay, outdir, jobs, script_lines, sde):
    """run script_lines (one shell command each, cwd /tmp/t) in one container."""
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'jobs.txt'), 'w', newline='\n') as f:
        for ln in script_lines:
            f.write(ln.replace('\n', ' ') + '\n')
    inner = ('set -e; cp -a /src /tmp/t; %s cd /tmp/t; '
             'tr "\\n" "\\0" < /out/jobs.txt | xargs -0 -P %d -n 1 sh -c' % (
                 'cp -a /ovl/. /tmp/t/;' if overlay else '', jobs))
    cmd = ['docker', 'run', '--rm', '--user', '%d:%d' % (os.getuid(), os.getgid())]
    if sde:
        cmd += ['-e', 'SOURCE_DATE_EPOCH=%s' % sde]
    cmd += ['-v', '%s:/src:ro' % tree, '-v', '%s:/out' % outdir]
    if overlay:
        cmd += ['-v', '%s:/ovl:ro' % overlay]
    cmd += ['-w', '/', image, 'sh', '-c', inner]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr)[-2000:]


# ------------------------------------------------------------------ files
def tree_files(root):
    out = {}
    for d, dirs, fns in os.walk(root):
        dirs[:] = [x for x in dirs if x != '.git']
        for fn in fns:
            if fn.endswith(SKIP_EXT) or fn.startswith('.build-'):
                continue
            p = os.path.join(d, fn)
            out[os.path.relpath(p, root).replace('\\', '/')] = p
    return out


def changed_files(ref, new, overlay):
    a, b = tree_files(ref), tree_files(new)
    if overlay:
        b.update(tree_files(overlay))
    ch = []
    for rel in sorted(set(a) | set(b)):
        pa, pb = a.get(rel), b.get(rel)
        if pa is None or pb is None or open(pa, 'rb').read() != open(pb, 'rb').read():
            ch.append(rel)
    return ch


INC = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def includers(changed_rel, tus, side):
    """TUs (gpsp-relative) that include changed_rel textually (closure by basename,
    conditions ignored: an over-approximation) or force-include it (-include)."""
    target = os.path.basename(changed_rel)
    memo = {}

    def reaches(rel, depth=0):
        if rel in memo:
            return memo[rel]
        memo[rel] = False
        p = side.path(rel)
        if depth > 12 or not os.path.exists(p):
            return False
        txt = open(p, encoding='utf-8', errors='replace').read()
        for inc in INC.findall(txt):
            if os.path.basename(inc) == target:
                memo[rel] = True
                return True
            for d in (os.path.dirname(rel), 'psp', 'frontend-common', 'netdrv', '.', 'libretro/libretro-common/include'):
                q = os.path.normpath(os.path.join(d, inc)).replace('\\', '/')
                if os.path.exists(side.path(q)) and reaches(q, depth + 1):
                    memo[rel] = True
                    return True
        return False
    out = []
    for tu, line in tus.items():
        if re.search(r'-include\s+%s\b' % re.escape(target), line) or reaches(tu):
            out.append(tu)
    return out


# ------------------------------------------------------------------ main
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ref', required=False); ap.add_argument('--new', required=False)
    ap.add_argument('--logs'); ap.add_argument('--overlay'); ap.add_argument('--keep')
    ap.add_argument('--allow-prune', action='append', default=[])
    ap.add_argument('--arms'); ap.add_argument('--flavours')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--max-order', type=int, default=2)
    ap.add_argument('--sde', default='1793908328')
    ap.add_argument('--image', default=os.environ.get('DOCKER_IMAGE', 'pspdev/pspdev'))
    ap.add_argument('--jobs', type=int, default=8)
    o = ap.parse_args(argv)
    if not (o.ref and o.new and o.logs):
        die('--ref, --new and --logs are required')
    ref, new = os.path.abspath(o.ref), os.path.abspath(o.new)
    ovl = os.path.abspath(o.overlay) if o.overlay else None
    for t in (ref, new):
        if not os.path.isfile(os.path.join(t, 'tools/build.sh')):
            die('%s has no tools/build.sh' % t)
    allow = set(o.allow_prune)
    pruned, allowed = [], []

    def skip(name, why):
        (allowed if name in allow else pruned).append('%s(%s)' % (name, why))

    work = tempfile.mkdtemp(prefix='ppsweep.')
    try:
        return run(o, ref, new, ovl, work, skip, pruned, allowed)
    finally:
        if o.keep:
            os.makedirs(o.keep, exist_ok=True)
            subprocess.run(['cp', '-r', work + '/.', o.keep])
            print('kept in %s' % o.keep)
        shutil.rmtree(work, ignore_errors=True)


def run(o, ref, new, ovl, work, skip, pruned, allowed):
    sides = {'ref': Side('ref', ref, None, work), 'new': Side('new', new, ovl, work)}
    sw = {}
    for k, s in sides.items():
        sws, inputs, uncl = switches(os.path.join(s.dry, 'tools/build.sh'))
        if uncl:
            die('%s tools/build.sh reads %s, neither a switch (a digit domain) nor a known '
                'free-form input: classify it in pp_sweep.py FREEFORM or give it a case' % (k, ' '.join(uncl)))
        sw[k] = sws
    if sw['ref'] != sw['new']:
        die('the two trees have different build.sh switch sets: ref %s, new %s' % (sw['ref'], sw['new']))
    switches_ = sw['ref']
    print('pp_sweep: %d switches from tools/build.sh: %s' % (len(switches_), ' '.join(
        '%s{%s}%s' % (n, ','.join(v), '' if d == '' else '=%s' % d) for n, (d, v) in sorted(switches_.items()))))
    logs = os.path.abspath(o.logs)
    flavours = o.flavours.split() if o.flavours else list(FOUR)
    partial = bool(o.flavours and set(flavours) != set(FOUR)) or bool(o.arms)
    present = []
    for fl in flavours:
        if fl not in PROFILE:
            die('unknown flavour %s' % fl)
        if os.path.isfile(os.path.join(logs, fl, 'logs/.build-eboot.log')):
            present.append(fl)
        else:
            skip(fl, 'no %s/%s/logs/.build-eboot.log' % (logs, fl))
    # the flavour's base environment: its flavours.sh record (buildEnv), else rfuSched + extraEnv
    import json
    base_env = {}
    for fl in present:
        fj = os.path.join(logs, fl, 'flavour.json')
        env = {}
        j = json.load(open(fj)) if os.path.exists(fj) else {}
        if j.get('buildEnv'):
            for w in j['buildEnv'].split():
                k, _, v = w.partition('=')
                if k in switches_ or k in ('LAYOUT_PAD_BYTES', 'PSP_EXTRA', 'CORE_EXTRA', 'FE_EXTRA'):
                    env[k] = v
        else:
            env['RFU_SCHED'] = j.get('rfuSched', '0' if fl == 'release-stable' else '1')
            for w in (j.get('extraEnv') or '').split():
                k, _, v = w.partition('=')
                env[k] = v
            print('pp_sweep: [%s] no buildEnv in %s (built before hermetic flavours.sh): '
                  'base env assumed %s, checked against the log below' % (fl, fj, env))
        base_env[fl] = env
    # arms
    values = [(n, v) for n, (d, vs) in sorted(switches_.items()) for v in vs + ([''] if d == '' and '' not in vs else [])]

    def arms_for(fl):
        be = base_env[fl]
        cur = {n: be.get(n, switches_[n][0]) for n in switches_}
        alt = [(n, v) for n, v in values if v != cur[n]]
        out = [('base', {})]
        for r in range(1, o.max_order + 1):
            for combo in itertools.combinations(alt, r):
                if len({n for n, _ in combo}) < r:
                    continue
                out.append(('+'.join('%s=%s' % (n, v if v != '' else 'unset') for n, v in combo), dict(combo)))
        return out
    arms = {fl: arms_for(fl) for fl in present}
    if o.arms:
        want = set(o.arms.split()) | {'base'}
        for fl in present:
            arms[fl] = [a for a in arms[fl] if a[0] in want]
        unknown = want - {a[0] for fl in present for a in arms[fl]}
        if unknown:
            die('unknown arm(s) %s (pp_sweep.sh --list shows them)' % ' '.join(sorted(unknown)))
    if o.list:
        for fl in present:
            print('[%s] %d arms: %s' % (fl, len(arms[fl]), ' '.join(a for a, _ in arms[fl])))
        return 0
    # dry build.sh -> make commands
    jobs = []
    for sn, s in sides.items():
        for fl in present:
            for name, delta in arms[fl]:
                env = dict(base_env[fl]); env.update(delta)
                env = {k: v for k, v in env.items() if v != ''}
                jobs.append((sn, fl, name, env))
    cache = {}

    def dry(job):
        sn, fl, name, env = job
        s = sides[sn]
        key = (s.recipe, PROFILE[fl], tuple(sorted(env.items())))
        tag = hashlib.sha1(repr((sn,) + key).encode()).hexdigest()[:16]
        return job, key, s.make_cmd(PROFILE[fl], env, tag)
    with ThreadPoolExecutor(o.jobs) as ex:
        res = list(ex.map(dry, jobs))
    mk = {}      # (side, fl, arm) -> make command
    for (sn, fl, name, env), key, (cmd, err) in res:
        if cmd is None:
            skip('%s:%s' % (fl, name), 'build.sh stopped before the psp make: %s' % err.strip().split('\n')[-1][:120])
            continue
        mk[(sn, fl, name)] = cmd
    # make -n -B per distinct command, one container per side
    lines = {}   # (side, cmd) -> {src_rel_psp: compile line}
    for sn, s in sides.items():
        cmds = sorted({c for (x, _, _), c in mk.items() if x == sn})
        ids = {c: hashlib.sha1(c.encode()).hexdigest()[:16] for c in cmds}
        od = os.path.join(work, 'mkn_' + sn)
        script = ['cd /tmp/t/psp && %s > /out/%s.txt 2>&1 || true' % (re.sub(r'^make ', 'make -n -B ', c), ids[c]) for c in cmds]
        rc, out = docker_batch(o.image, s.tree, s.overlay, od, o.jobs, script, o.sde)
        if rc != 0:
            die('make -n container failed on %s: %s' % (sn, out))
        for c in cmds:
            p = os.path.join(od, ids[c] + '.txt')
            d = {}
            for ln in open(p, encoding='utf-8', errors='replace').read().split('\n'):
                m = LINE_RE.search(ln.rstrip())
                if m and ln.startswith('psp-'):
                    src = os.path.normpath(os.path.join('psp', m.group(2))).replace('\\', '/')
                    d[src] = ln.rstrip()
            if 'psp/main_psp.c' not in d:
                die('make -n on %s gave no main_psp.o line for: %s' % (sn, c[:200]))
            lines[(sn, c)] = d
    # the derivation is the build: the derived base lines equal the logged ones
    for fl in present:
        c = mk.get(('ref', fl, 'base'))
        if c is None:
            die('[%s] no base make command' % fl)
        logged = {}
        for ln in open(os.path.join(logs, fl, 'logs/.build-eboot.log'), encoding='utf-8', errors='replace'):
            m = LINE_RE.search(ln.rstrip())
            if m and ln.startswith('psp-'):
                logged[os.path.normpath(os.path.join('psp', m.group(2))).replace('\\', '/')] = ln.rstrip()
        der = lines[('ref', c)]
        bad = [t for t in der if t in logged and der[t] != logged[t]]
        if 'psp/main_psp.c' not in logged:
            die('[%s] the log has no main_psp.o line' % fl)
        # every derived TU must be in the log (sub-makes, me/ and home/, are logged but
        # not derived: their objects are not the frontend's TUs and are never swept)
        if set(der) - set(logged):
            die('[%s] derived TUs missing from the log: %s' % (fl, sorted(set(der) - set(logged))[:8]))
        if bad:
            t = 'psp/main_psp.c' if 'psp/main_psp.c' in bad else bad[0]
            ds = lambda s: set(re.findall(r' (-[DU]\S+)', s))
            die('[%s] the derived %s line is not the logged one (the run was built with another '
                'environment, or the derivation is wrong): derived-only %s, logged-only %s'
                % (fl, t, sorted(ds(der[t]) - ds(logged[t])), sorted(ds(logged[t]) - ds(der[t]))))
        print('pp_sweep: [%s] derived compile lines = the logged ones (%d TUs)' % (fl, len(der)))
    # which files
    ch = changed_files(ref, new, ovl)
    any_lines = lines[('new', mk[('new', present[0], 'base')])]
    tus_new = {t: l for t, l in any_lines.items()}
    sweep = {'psp/main_psp.c'}
    for rel in ch:
        if IGNORED.search(rel):
            print('pp_sweep: changed %s: not a build input (ignored)' % rel)
        elif RECIPE.match(rel):
            print('pp_sweep: changed %s: build recipe, each side derives its own lines' % rel)
        elif rel in tus_new or any(rel in lines[k] for k in lines):
            sweep.add(rel)
        elif rel.endswith(('.h', '.inc', '.inc.c', '.c', '.cc')):
            inc = includers(rel, tus_new, sides['new']) + includers(rel, tus_new, sides['ref'])
            if inc:
                sweep.update(inc)
                print('pp_sweep: changed %s: swept through %s' % (rel, ' '.join(sorted(set(inc)))))
            else:
                skip(rel, 'changed, but no psp TU includes it: outside the -E view')
        else:
            skip(rel, 'changed, outside the -E view (not a C source, not the recipe)')
    print('pp_sweep: %d changed file(s); files swept: %s' % (len(ch), ' '.join(sorted(sweep))))
    # the -E jobs
    pre = {}     # side -> {id: -E command}
    want = []    # (fl, arm, file, id_ref, id_new)
    diffs_pre = []   # a TU compiled on one side only
    for fl in present:
        for name, _ in arms[fl]:
            ids = {sn: mk.get((sn, fl, name)) for sn in sides}
            if None in ids.values():
                continue                      # already a skip (build.sh stopped)
            for f in sorted(sweep):
                got = {sn: lines[(sn, ids[sn])].get(f) for sn in ('ref', 'new')}
                if got['ref'] is None and got['new'] is None:
                    if f == 'psp/main_psp.c':
                        skip('%s:%s:%s' % (fl, name, f), 'no compile line for main_psp.c')
                    continue                  # this TU is not built in this arm, on either side
                if None in got.values():
                    diffs_pre.append((fl, name, f, 'compiled on %s only' % ('ref' if got['ref'] else 'new')))
                    continue
                pair = []
                for sn in ('ref', 'new'):
                    ppl = got[sn].replace(' -MMD -MP ', ' ')
                    m = LINE_RE.search(ppl)
                    i = hashlib.sha1(('%s|%s' % (f, ppl)).encode()).hexdigest()[:20]
                    pre.setdefault(sn, {})[i] = ppl[:m.start()] + ' -E -P -o /out/%s.i %s' % (i, m.group(2))
                    pair.append(i)
                want.append((fl, name, f, pair[0], pair[1]))
    for sn, s in sides.items():
        od = os.path.join(work, 'pp_' + sn)
        script = ['cd /tmp/t/psp && %s > /out/%s.err 2>&1 || echo fail > /out/%s.fail' % (l, i, i)
                  for i, l in sorted(pre.get(sn, {}).items())]
        rc, out = docker_batch(o.image, s.tree, s.overlay, od, o.jobs, script, o.sde)
        if rc != 0:
            die('preprocessing container failed on %s: %s' % (sn, out))
    ntok = {}

    def tok(sn, i):
        k = (sn, i)
        if k not in ntok:
            p = os.path.join(work, 'pp_' + sn, i + '.i')
            if os.path.exists(os.path.join(work, 'pp_' + sn, i + '.fail')) or not os.path.exists(p):
                err = open(os.path.join(work, 'pp_' + sn, i + '.err'), errors='replace').read()[-400:] \
                    if os.path.exists(os.path.join(work, 'pp_' + sn, i + '.err')) else ''
                ntok[k] = (None, err)
            else:
                raw = open(p, 'rb').read().decode('latin-1')
                ntok[k] = (norm(raw), raw)
        return ntok[k]
    rc, diffs, errors, ok = 0, [], [], 0
    for fl, name, f, why in diffs_pre:
        diffs.append((fl, name, f))
        print('  DIFFERENT %s arm %s %s: %s' % (fl, name, f, why))
    seen_pairs = {}
    for fl, name, f, ir, inn in want:
        a, ra = tok('ref', ir)
        b, rb = tok('new', inn)
        if a is None or b is None:
            errors.append('%s:%s:%s %s' % (fl, name, f, (ra if a is None else rb).strip().split('\n')[-1][:160]))
            continue
        if not a or not b:
            errors.append('%s:%s:%s empty stream' % (fl, name, f))
            continue
        if a == b:
            ok += 1
            continue
        diffs.append((fl, name, f))
        key = (ir, inn)
        if key in seen_pairs:
            continue
        seen_pairs[key] = 1
        la = [re.sub(r'\s+', ' ', x).strip() for x in ra.split('\n') if x.strip()]
        lb = [re.sub(r'\s+', ' ', x).strip() for x in rb.split('\n') if x.strip()]
        import difflib
        d = list(difflib.unified_diff(la, lb, lineterm='', n=1))[2:30]
        print('  DIFFERENT %s arm %s %s:' % (fl, name, f))
        for x in d:
            print('      ' + x[:200])
    narms = sum(len(arms[fl]) for fl in present)
    print('pp_sweep: %d flavour(s) x arms = %d arms, %d files; %d (arm, file) pairs, %d distinct -E inputs per side; '
          '%d identical, %d different, %d errors' % (len(present), narms, len(sweep), len(want),
                                                     len(pre.get('new', {})), ok, len(diffs), len(errors)))
    for e in errors[:20]:
        print('  ERROR %s' % e)
    if diffs:
        by = {}
        for fl, name, f in diffs:
            by.setdefault(name, []).append('%s:%s' % ({'release': 'rel', 'release-stable': 'stb', 'lab': 'lab', 'harness': 'har'}[fl], os.path.basename(f)))
        print('pp_sweep: DIFFERENT in %d arm(s): %s' % (len(by), '; '.join('%s [%s]' % (k, ','.join(v)) for k, v in sorted(by.items())[:40])))
    print('pp_sweep: pruned %s; allowed prunes %s' % (' '.join(pruned) or 'none', ' '.join(allowed) or 'none'))
    if errors or pruned:
        rc = 2
    elif diffs:
        rc = 1
    elif partial:
        rc = 3
    verdict = {0: 'G0var ok x%d arms (%s)' % (narms, '/'.join(present)), 1: 'G0var FAIL (different)',
               2: 'G0var FAIL (%s)' % ('skipped arms/files' if pruned else 'errors'), 3: 'G0var PARTIAL (subset: not a gate)'}[rc]
    print(verdict)
    return rc


if __name__ == '__main__':
    sys.exit(main())
