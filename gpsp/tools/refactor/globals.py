#!/usr/bin/env python3
"""globals.py -- T9 (builds/mainpsp-map/out/PLAN.md §5, §1.3): the globals census
of psp/main_psp.c, fixed to parse `__attribute__` declarations, with lifetime
class, owner module, fragment and cross-module writers; plus a safe rename.

  globals.py census [--src main_psp.c] [--root GPSP] [--cutlist cutlist.txt]
                    [--json OUT] [--tsv OUT] [--basis vars_g.json]
        Census of every file-scope definition.  Exit 1 if any definition has
        no owner module (G99).  --basis compares with x1's census: the names
        it lacked must be exactly the attributed ones.
  globals.py rename OLD NEW [--src main_psp.c] -o OUT
        Word-boundary rename of a file-scope global, in CODE tokens only
        (comments and string literals are left alone).  REFUSES (exit 1,
        nothing written) when: an occurrence is inside fe_evt(...) /
        LAB_HOOK(...) / LAB_FFP(...) / FE_LP_SET(...) arguments (r6 B3: those
        compile out in the release, so a rename there is checked by no
        release build); OLD is exported ABI or used by another TU (PLAN §1.3)
        or a weak-override hook; NEW already names something; OLD is not a
        file-scope global of the file; OLD is also a local/parameter somewhere
        (shadowed: a token rename would rename the local too).
  globals.py --selftest      tests/source-split_test_globals.py

Basis: scratch/census.py (parse, now csrc.py with the attribute fix),
scratch/gen.py (lifetime rules, x1 §E), scratch/md.py (x1 groups; modules.py
extends them for the 23 the x1 parser missed), vfy_x1.py (the ELF cross-check
of x1 Verification: see --elf).
"""
import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import csrc      # noqa: E402
import modules   # noqa: E402

# The release and rig define sets.  The fixed part is what build.sh's profiles pass
# (EXTRA_DEFS) plus fe_evt.h's derived GPSP_NO_TELEMETRY; the switch part (REWIND,
# GBA_LINK, GBADHOC_ONLINE, GBADHOC_MGIFT, GPSP_CHEATS, ...) is DERIVED from the tree's
# psp/Makefile (makefile_defines: its `X ?= default` + `ifeq ($(X),1)` + `CFLAGS +=
# -D...`), so a switch a merge adds (round 10's MGIFT) is not silently missing (P0a,
# review 2 #7).  A flavours.sh build dir can be given instead (--defs-from): its
# build-manifest.json pspMakeFlags override the Makefile defaults.
REL_FIXED = {'GPSP_PLAYABLE': 1, 'GPSP_NO_TELEMETRY': 1, 'PSP': 1, 'ME_MIDFRAME_LOG': 1}
RIG_FIXED = {'GPSP_PLAYABLE': 1, 'GPSP_KEEP_TELEMETRY': 1, 'GPSP_PERF_RIG': 1, 'PSP': 1, 'ME_MIDFRAME_LOG': 1}
# 01067e74's sets (the tools' own proof, before P0a); the self-test checks that the
# Makefile-derived sets equal these on 01067e74 for every name its main_psp.c tests
REL = dict(REL_FIXED, REWIND=1, GBA_LINK=1, GBADHOC_ONLINE=1)
RIG = dict(RIG_FIXED, REWIND=1, GBA_LINK=1, GBADHOC_ONLINE=1)
# the main_psp.c sha256 the fixed modules.py ranges were written for (the header of
# tests/fixtures/cutlist_01067e74.txt; tools/refactor/cutlist.txt follows the tree since P1)
SHA_01067E74 = '942c8b546d6b159bf80660081fde816e31082a99aa6e4442a8b45e8272fe5cd1'


def makefile_defines(makefile, overrides=None):
    """-> {NAME: value} of the `CFLAGS += -DNAME[=V]` a psp/Makefile adds with its
    defaults (`X ?= v`), `overrides` ({VAR: value}, e.g. a manifest's pspMakeFlags)
    taking precedence, through ifeq/ifneq/else/endif.  A condition it cannot evaluate
    makes that branch inactive (reported in the second return value)."""
    var = {}
    over = dict(overrides or {})
    out, unknown = {}, []
    stack = []        # [(active_parent, taken, active)]
    active = True

    def val(x):
        x = x.strip()
        m = re.fullmatch(r'\$\((\w+)\)', x)
        if m:
            return over.get(m.group(1), var.get(m.group(1), ''))
        return x

    for ln in open(makefile, encoding='utf-8', errors='replace'):
        t = ln.split('#', 1)[0].rstrip()
        st = t.strip()
        m = re.match(r'^(else\s+)?(ifeq|ifneq)\s*\(\s*([^,]*?)\s*,\s*(.*?)\s*\)\s*$', st)
        if m:
            ok = None
            a, b = m.group(3), m.group(4)
            if '$(' in a.replace('$(', '', 1) or '$(' in b or not re.fullmatch(r'\$\(\w+\)|[\w.-]*', a.strip()):
                unknown.append(st)
            else:
                ok = (val(a) == val(b)) == (m.group(2) == 'ifeq')
            if m.group(1):           # else ifeq: a chained branch of the current if
                par, taken, _ = stack[-1]
                now = par and not taken and bool(ok)
                stack[-1] = (par, taken or now, now)
            else:
                stack.append((active, bool(ok) and active, bool(ok) and active))
            active = stack[-1][2]
            continue
        m = re.match(r'^(ifdef|ifndef)\s+(\w+)$', st)
        if m:
            d = bool(over.get(m.group(2), var.get(m.group(2), '')))
            ok = d if m.group(1) == 'ifdef' else not d
            stack.append((active, ok and active, ok and active))
            active = stack[-1][2]
            continue
        if st == 'else':
            par, taken, _ = stack[-1]
            stack[-1] = (par, True, par and not taken)
            active = stack[-1][2]
            continue
        if st == 'endif':
            stack.pop()
            active = stack[-1][2] if stack else True
            continue
        if not active or ln[:1] == '\t':
            continue
        m = re.match(r'^(\w+)\s*\?=\s*(.*)$', st)
        if m:
            var.setdefault(m.group(1), m.group(2).strip())
            continue
        m = re.match(r'^(\w+)\s*:?=\s*(.*)$', st)
        if m and m.group(1) != 'CFLAGS':
            var[m.group(1)] = m.group(2).strip()
            continue
        m = re.match(r'^CFLAGS\s*\+?=\s*(.*)$', st)
        if m:
            for dm in re.finditer(r'-D(\w+)(?:=(\S+))?', m.group(1)):
                v = dm.group(2)
                if v is not None and '$(' in v:
                    vm = re.fullmatch(r'\$\((\w+)\)', v)
                    v = over.get(vm.group(1), var.get(vm.group(1), '')) if vm else v
                try:
                    out[dm.group(1)] = int(v, 0) if v is not None else 1
                except ValueError:
                    out[dm.group(1)] = 1
    return out, unknown


def define_sets(root, defs_from=None):
    """-> (REL, RIG, how) for a gpsp tree: fixed + Makefile-derived."""
    mk = os.path.join(root, 'psp', 'Makefile') if root else None
    if not mk or not os.path.exists(mk):
        return REL, RIG, 'fixed 01067e74 sets (no psp/Makefile under --root)'
    ov = {}
    how = 'psp/Makefile defaults'
    if defs_from:
        mf = os.path.join(defs_from, 'build-manifest.json')
        j = json.load(open(mf))
        for kv in j.get('pspMakeFlags', '').split():
            if '=' in kv:
                k, v = kv.split('=', 1)
                ov[k] = v
        how = 'psp/Makefile + %s pspMakeFlags' % mf
    md, _ = makefile_defines(mk, ov)
    rel = dict(REL_FIXED, **md)
    rig = dict(RIG_FIXED, **md)
    return rel, rig, how


def src_sha(path):
    import hashlib
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def cutlist_sha(path):
    for ln in open(path, encoding='utf-8'):
        if not ln.startswith('#'):
            break
        m = re.search(r'sha256 ([0-9a-f]{64})', ln)
        if m:
            return m.group(1)
    return None


# the census never counts main_psp.c's own text (the file, its P4 fragments and the
# P5 headers) as "another TU": by RULE, not by the one --src path, so a census of a
# copy or of a split tree classifies exactly like the in-tree one (MF10's cause:
# `--root` excluded only the exact --src path, so a copy saw the in-tree main_psp.c
# as an extern user of every global it defines)
SELF_TU = re.compile(r'^psp/(main_psp\.c|fe/.*)$')
FRAG_INCLUDE = re.compile(r'^\s*#\s*include\s+"fe/[^"]+\.inc\.c"', re.M)
SPAN_DEAD = ('LAB_HOOK', 'LAB_FFP', 'fe_evt', 'FE_LP_SET')
# PLAN §1.3: exported ABI (x1 H5) and the weak overrides (x1 H6) -- never renamed by this tool
EXPORTED = {'g_dir_base', 'g_running', 'g_ff_uncapped', 'g_ff_mult', 'in_rate', 'g_audio_step', 'g_net_up',
            'g_net_is_host', 'g_catch_selftest_addr', 'g_rfu_activated'}


# x1's census (and csrc.py before review 2026-11-05) also lost a definition of
# an ANONYMOUS aggregate type with an initializer: `static const struct {...}
# g_fxs_gba[] = {...};` (L:6773; g_fxs_gb the same shape after it; FX_SWEEP && GPSP_PERF_RIG only, so no built
# ELF holds it).  Not one of PLAN §5's 23 (those are the attributed ones).
X1_ANON_AGG = {'g_fxs_gba', 'g_fxs_gb'}


def ev(cond, flags):
    if not cond:
        return True
    e = cond
    e = re.sub(r'defined\s*\(\s*(\w+)\s*\)', lambda m: ' 1 ' if m.group(1) in flags else ' 0 ', e)
    e = re.sub(r'defined\s+(\w+)', lambda m: ' 1 ' if m.group(1) in flags else ' 0 ', e)
    e = e.replace('&&', ' and ').replace('||', ' or ')
    e = re.sub(r'!(?!=)', ' not ', e)
    e = re.sub(r'\b([A-Za-z_]\w*)\b', lambda m: m.group(1) if m.group(1) in ('and', 'or', 'not') else str(flags.get(m.group(1), 0)), e)
    e = re.sub(r'(\d+)[uUlL]+', r'\1', e)
    try:
        return bool(eval(e))
    except Exception:
        return None


HRX = re.compile(r'^(rig_|uiscript_|shash_|sleep_sim|stall_|heartbeat_|drprof_|boot_mark|boot_timeline|boot_mem_census|boot_ballast|'
                 r'pad_script_|dump_|vhash_|eboot_crc|vram_probe|vid_prof|fx_sweep|sessmem_|gblink_script_|run_testpat|run_nettest|'
                 r'heap_census|mer_note_sum|lat_census|ff_win_note|psp_clock_harness|jit_coh_|frame_hist_|preempt_evt|boost_evt|'
                 r'audio_ring_note|plat_audio_hash|plat_audio_sample_count|core_health_evt|smc_gates_reload_evt|blit_prof_evt|'
                 r'sess_cost_evt|adhoc_ctl_evt|adhoc_stats_evt|net_error_evt|evt_|vprof_clock|audio_status_frame|link_version_text)')


def is_h(fn):
    return bool(HRX.match(fn)) or fn.endswith('_evt')


def main_phases(d):
    """session_top / loop start / loop end, from anchors (gen.py hard-coded 15251/16368/18070)."""
    raw = d['raw']
    mf = [f for f in d['funcs'] if f['name'] == 'main'][0]
    clean = d['clean']
    st = re.compile(r'^session_top:', re.M).search(raw, mf['s'])
    session_top = d['lineof'](st.start()) if st else mf['start']
    lw = re.compile(r'^   while \(g_running\)\s*\n   \{', re.M).search(raw, st.end() if st else mf['s'])
    loop0 = d['lineof'](lw.start())
    j = clean.index('{', lw.start())
    dep = 0
    while True:
        if clean[j] == '{':
            dep += 1
        elif clean[j] == '}':
            dep -= 1
            if dep == 0:
                break
        j += 1
    return session_top, loop0, d['lineof'](j)


def census(src, root=None, cutlist=None, defs_from=None):
    global REL, RIG
    raw = open(src, encoding='utf-8', newline='').read()
    if FRAG_INCLUDE.search(raw):
        raise SystemExit(2)    # main() prints why (refuse_split)
    REL, RIG, defs_how = define_sets(root, defs_from)
    d = csrc.parse(raw, root)
    V, F, REFS, CALLS, LC = d['vars'], d['funcs'], d['refs'], d['calls'], d['line_cond']
    fby = collections.defaultdict(list)
    for f in F:
        fby[f['name']].append(f)

    def line_rel(ln):
        return ev(LC[ln] or '', REL)

    # extern users across the tree (gen.py)
    users = collections.defaultdict(set)
    if root:
        nameset = {v['name'] for v in V}
        selfp = os.path.normpath(os.path.abspath(src))
        for dp, dn, fn in os.walk(root):
            dn[:] = [x for x in dn if x not in ('.git', '__pycache__')]
            for x in fn:
                if not x.endswith(('.c', '.h', '.inc', '.S', '.s', '.cpp')):
                    continue
                p = os.path.normpath(os.path.abspath(os.path.join(dp, x)))
                if p == selfp or SELF_TU.match(os.path.relpath(p, root).replace('\\', '/')):
                    continue
                try:
                    txt = open(p, encoding='utf-8', errors='replace').read()
                except OSError:
                    continue
                txt = re.sub(r'/\*.*?\*/', ' ', txt, flags=re.S)
                txt = re.sub(r'//[^\n]*', ' ', txt)
                for t in set(re.findall(r'\b[A-Za-z_]\w*\b', txt)) & nameset:
                    users[t].add(os.path.relpath(p, root).replace('\\', '/'))

    ENTRY = {'cb': ['cb_thread', 'exit_cb', 'power_cb'], 'io': ['io_thread'], 'audio': ['audio_thread'],
             'boost': ['boost_warden'], 'stall(H)': ['stall_thread'], 'drprof(H)': ['drprof_sampler'],
             'sleepsim(H)': ['sleep_sim_thread']}

    def reach(roots, stop=()):
        seen = set()
        stck = list(roots)
        while stck:
            x = stck.pop()
            if x in seen or x in stop:
                continue
            seen.add(x)
            stck.extend(CALLS.get(x, []))
        return seen
    allentries = tuple(x for v in ENTRY.values() for x in v)
    ctx = {k: reach(v, stop=('main',)) for k, v in ENTRY.items()}
    mainctx = reach(['main'], stop=allentries)
    for nm, lst in d['fnref'].items():
        if any(f == 'main' for (_, f) in lst) or nm.startswith('sq_') or nm in ('io_wake', 'io_yield', 'psp_rw_pix_begin', 'psp_rw_pix_end', 'psp_rw_show'):
            mainctx |= reach([nm], stop=allentries)
    for f in F:
        if not f['static'] and f['name'] != 'main':
            mainctx |= reach([f['name']], stop=allentries)
    ctx['main'] = mainctx
    CTX_REL = {'main'} | {k for k, v in ENTRY.items() if any(line_rel(f['body']) for f in fby.get(v[0], []))}

    SESSION_TOP, LOOP0, LOOP1 = main_phases(d)
    vnames = {v['name'] for v in V if not v['extern']}
    MACRO_TWINS = collections.defaultdict(list)
    for k, ln in enumerate(raw.split('\n'), 1):
        mm = re.match(r'^\s*#\s*define\s+(\w+)(?!\()\s', ln)
        if mm and mm.group(1) in vnames and d['pp_depth'][k] > 0:
            MACRO_TWINS[mm.group(1)].append((LC[k] or '').strip())

    def phase_of_line(ln):
        if ln < SESSION_TOP:
            return 'boot'
        if ln < LOOP0:
            return 'session'
        if ln <= LOOP1:
            return 'loop'
        return 'teardown'
    phase_fn = collections.defaultdict(set)
    for (ln, callee, span) in d['mcalls']:
        if span in ('LAB_HOOK', 'LAB_FFP', 'fe_evt') or not line_rel(ln):
            continue
        for r in d['macro_calls'].get(callee, [callee]):
            for x in reach([r], stop=('main',)):
                phase_fn[x].add(phase_of_line(ln))

    # module of each function: the cutlist when given, else modules.py
    fmod, ffrag, gfrag, gmod = {}, {}, {}, {}
    if cutlist:
        sys.path.insert(0, HERE)
        import split as _split
        _, rows = _split.read_cutlist(cutlist)
        for r in rows:
            if r['kind'] == 'F':
                fmod.setdefault(r['name'], r['module'])
                ffrag.setdefault(r['name'], r['frag'])
            else:
                gfrag[r['key']] = r['frag']
                gmod[r['key']] = r['module']
    else:
        for f in F:
            fmod.setdefault(f['name'], modules.function_module(f['name'], f['start'])[0])

    out = []
    occ = collections.Counter()
    for v in V:
        nm = v['name']
        recs = REFS.get(nm, {'lines': []})['lines']
        rel_decl, rig_decl = ev(v['cond'], REL), ev(v['cond'], RIG)
        live = [x for x in recs if line_rel(x[0]) and x[3] not in SPAN_DEAD]
        rd, wr, ad = collections.Counter(), collections.Counter(), collections.Counter()
        for (ln, fn, kind, span) in live:
            if kind in ('r', 'rw'):
                rd[fn] += 1
            if kind in ('w', 'rw'):
                wr[fn] += 1
            if kind == 'a':
                ad[fn] += 1
        th = set()
        for (ln, fn, kind, span) in live:
            for c, s in ctx.items():
                if c in CTX_REL and fn in s:
                    th.add(c)
        pure_r = [x for x in live if x[2] in ('r', 'a')]
        ext_real = sorted(u for u in users.get(nm, ()) if not u.startswith('tools/'))
        ph = set()
        for (ln, fn, kind, span) in live:
            if kind in ('w', 'rw', 'a'):
                if fn == 'main':
                    ph.add(phase_of_line(ln))
                else:
                    ph |= phase_fn.get(fn, set())
                    for c in ctx:
                        if c != 'main' and fn in ctx[c]:
                            ph.add('async:' + c)
        readers, writers = set(rd) | set(ad), set(wr) | set(ad)
        hfun = all(is_h(f) for f in (readers | writers)) if (readers | writers) else False
        if v['extern']:
            life = 'import'
        elif not rel_decl:
            life = 'H-cut' if rig_decl else 'variant'
        elif not live:
            life = 'dead-rel' if recs else 'dead'
        elif v.get('const') or (not writers and not ext_real):
            life = 'const' if not writers else 'boot'     # gen.py: const-declared but written -> boot-forever
        elif not pure_r and v['static'] and not ext_real:
            life = 'WO-rel'
        elif hfun:
            life = 'H-rel'
        elif any(p == 'loop' or p.startswith('async') for p in ph):
            life = 'loop'
        elif 'session' in ph or 'teardown' in ph:
            life = 'session'
        else:
            life = 'boot'
        grp = modules.group_of(v)
        owner, how = modules.owner_of(v, grp)
        key = None
        if not v['extern']:
            occ[nm] += 1
            key = 'G:%s#%d' % (nm, occ[nm])
        # the owner is the cutlist's G row (the reviewed source of truth); modules.py's
        # opinion is kept and a disagreement is reported (and fails the census)
        mod_owner = owner
        if key in gmod:
            owner, how = gmod[key], 'cutlist'
        disagree = bool(key in gmod and mod_owner != gmod[key])
        # a file-scope macro twin: `#define NAME ...` in another #if arm of a static
        # object NAME (C15: g_start_*, g_reload_slow_ms, g_gblink_host_wait_peer,
        # g_mgift_consumed, g_mg_on) -- the same item, tagged with the macro's arm
        macro_in = sorted(set(MACRO_TWINS.get(nm, ())))
        # writers (all refs, release or not) whose module is not the owner
        allw = sorted({fn for (ln, fn, kind, span) in recs if kind in ('w', 'rw', 'a')})
        xmod = sorted('%s(%s)' % (fn, fmod.get(fn)) for fn in allw if owner and fmod.get(fn) not in (owner, None))
        out.append({'name': nm, 'key': key, 'line': v['line'], 'extern': v['extern'], 'static': v['static'],
                    'volatile': v['volatile'], 'const': v['const'], 'type': v['type'], 'size': v['size'],
                    'attrs': v['attrs'], 'section': v['section'], 'cond': v['cond'], 'rel': rel_decl, 'rig': rig_decl,
                    'group': grp, 'owner': owner, 'owner_how': how, 'fragment': gfrag.get(key),
                    'modules_owner': mod_owner, 'owner_disagrees': disagree,
                    'macro_in': ['macro-in:' + (c or 'always') for c in macro_in],
                    'life': life, 'threads': sorted(th), 'phases': sorted(ph),
                    'readers': sorted(readers), 'writers': sorted(writers), 'xmod_writers': xmod,
                    'ext_users': ext_real, 'nrefs': len(recs), 'nlive': len(live)})
    return out, {'session_top': SESSION_TOP, 'loop0': LOOP0, 'loop1': LOOP1, 'ctx_rel': sorted(CTX_REL),
                 'funcs': len(F), 'd': d, 'defs_how': defs_how, 'rel': REL, 'rig': RIG,
                 'cutlist': cutlist}


def report(rows, meta, basis=None):
    defs = [r for r in rows if not r['extern']]
    imports = [r for r in rows if r['extern']]
    attributed = [r for r in defs if r['attrs']]
    un = [r for r in defs if r['owner'] is None]
    print('globals census: %d file-scope declarations: %d definitions + %d extern imports; %d distinct defined names'
          % (len(rows), len(defs), len(imports), len({r['name'] for r in defs})))
    print('  main() phases from anchors: session_top L:%d, loop L:%d-%d (x1 used 15251 / 16368-18070)'
          % (meta['session_top'], meta['loop0'], meta['loop1']))
    print('  attributed definitions (the x1 parser dropped them): %d' % len(attributed))
    for r in attributed:
        print('    L:%-6d %-18s %-8s %s' % (r['line'], r['name'], r['section'] or '', ' '.join(a for a in r['attrs'])[:60]))
    rel = [r for r in defs if r['rel']]
    print('  compiled into the release: %d' % len(rel))
    lc = collections.Counter(r['life'] for r in rel)
    print('  release lifetime classes: ' + ', '.join('%s %d' % kv for kv in lc.most_common()))
    oc = collections.Counter(r['owner'] for r in defs)
    print('  owner modules: ' + ', '.join('%s %d' % kv for kv in sorted(oc.items(), key=lambda kv: str(kv[0]))))
    xm = [r for r in defs if r['xmod_writers']]
    print('  globals written from another module\'s function: %d' % len(xm))
    mis = [r for r in defs if r['fragment'] and r['owner'] and r['fragment'].split('.')[0] != r['owner']]
    if any(r['fragment'] for r in defs):
        print('  definitions sitting in another module\'s fragment (to move in epoch C, not in P4): %d' % len(mis))
    print('  UNASSIGNED (no owner module): %d%s' % (len(un), (': ' + ', '.join(r['name'] for r in un)) if un else ''))
    print('  define sets: %s; release has%s GBADHOC_MGIFT' % (meta.get('defs_how', '?'),
                                                             '' if meta.get('rel', {}).get('GBADHOC_MGIFT') else ' NO'))
    mt = sorted({r['name'] for r in defs if r.get('macro_in')})
    print('  macro-globals (an object in one arm, a #define in another): %d%s' % (len(mt), (': ' + ', '.join(mt)) if mt else ''))
    dis = [r for r in defs if r.get('owner_disagrees')]
    if meta.get('cutlist'):
        print('  owners from the cutlist (%s); modules.py disagrees on %d%s' % (
            os.path.basename(meta['cutlist']), len(dis),
            (': ' + ', '.join('%s (cutlist %s, modules.py %s)' % (r['name'], r['owner'], r['modules_owner']) for r in dis)) if dis else ''))
    rc = 1 if un or dis else 0
    if basis:
        b = json.load(open(basis))
        bk = {(x['name'], x['line'], x['extern']) for x in b}
        nk = {(r['name'], r['line'], r['extern']) for r in rows}
        lost = sorted(bk - nk)
        new = sorted(nk - bk)
        new_defs = [k for k in new if not k[2]]
        attr_keys = {(r['name'], r['line'], r['extern']) for r in rows if r['attrs']}
        # new and not attributed: allowed only for the definitions x1 lost to a
        # SECOND parser bug (an anonymous aggregate type with an initializer,
        # fixed in csrc.py on review 2026-11-05)
        unexpl = [k for k in new if k not in attr_keys and k[0] not in X1_ANON_AGG]
        print('  vs x1 basis %s: %d of its %d entries kept, %d lost; %d new (%d definitions, %d externs): '
              '%d attributed, %d anonymous-aggregate (%s), %d unexplained'
              % (os.path.basename(basis), len(bk) - len(lost), len(bk), len(lost), len(new),
                 len(new_defs), len(new) - len(new_defs), sum(1 for k in new if k in attr_keys),
                 sum(1 for k in new if k[0] in X1_ANON_AGG), ','.join(sorted(X1_ANON_AGG)), len(unexpl)))
        if lost or unexpl:
            rc = 1
    return rc


# ------------------------------------------------------------------ rename
class Refused(Exception):
    pass


def rename(text, old, new, root=None):
    d = csrc.parse(text, root)
    gl = [v for v in d['vars'] if v['name'] == old]
    if not gl or all(v['extern'] for v in gl):
        raise Refused('%s is not a file-scope global defined in this file' % old)
    if old in EXPORTED or old.startswith('gpsp_'):
        raise Refused('%s is exported ABI / a weak-override hook (PLAN §1.3): never renamed' % old)
    if not re.match(r'^[A-Za-z_]\w*$', new):
        raise Refused('%r is not an identifier' % new)
    clean = d['clean']
    if re.search(r'\b%s\b' % re.escape(new), clean) or new in d['defines']:
        raise Refused('%s already names something in this file' % new)
    if old in d['shadow']:
        raise Refused('%s is shadowed by a local or parameter in %s' % (old, ', '.join(sorted(set(d['shadow'][old])))))
    # review 2026-11-05: csrc blanks preprocessor lines, so a use inside a
    # `#define` body was neither renamed nor refused (the macro kept the old
    # name and broke whichever flavour expands it).  Refuse instead.
    bc = csrc.blank_comments_strings(text).split('\n')
    in_pp = [k + 1 for k, ln in enumerate(bc) if d['pp_line'][k + 1] and re.search(r'\b%s\b' % re.escape(old), ln)]
    if in_pp:
        raise Refused('%s occurs in a preprocessor directive (a #define body or #if), which a token rename does not '
                      'cover: L:%s' % (old, ','.join(map(str, in_pp))))
    # external linkage: "used by another TU" (docstring, PLAN §1.3) was not
    # checked.  A non-static definition is refused when any other source of
    # the tree names it (or when no tree is given to prove it is unused).
    if any(not v['extern'] and not v['static'] for v in gl):
        if not root:
            raise Refused('%s has external linkage and no --root was given to prove no other TU uses it' % old)
        others = []
        selfn = 'main_psp.c'
        for dp, dn, fn in os.walk(root):
            dn[:] = [x for x in dn if x not in ('.git', '__pycache__', 'tools')]
            for x in fn:
                if x.endswith('.inc.c') and os.path.basename(dp) == 'fe':
                    continue      # a fragment of main_psp.c itself (same TU)
                if x.endswith(('.c', '.h', '.inc', '.S', '.s', '.cpp', '.ld', '.x')) and x != selfn:
                    try:
                        t = open(os.path.join(dp, x), encoding='utf-8', errors='replace').read()
                    except OSError:
                        continue
                    if re.search(r'\b%s\b' % re.escape(old), t):
                        others.append(os.path.relpath(os.path.join(dp, x), root).replace('\\', '/'))
        if others:
            raise Refused('%s has external linkage and another TU names it: %s' % (old, ', '.join(sorted(others)[:8])))
    hits = [m.start() for m in re.finditer(r'\b%s\b' % re.escape(old), clean)]
    bad = []
    for p in hits:
        for (a, b, kind) in d['spans']:
            if a <= p <= b:
                bad.append('L:%d in %s(...)' % (d['lineof'](p), kind))
                break
    if bad:
        raise Refused('%s occurs inside telemetry/Lab macro arguments, which the release compiles out (r6 B3): %s'
                      % (old, ', '.join(bad)))
    out = []
    last = 0
    for p in hits:
        out.append(text[last:p])
        out.append(new)
        last = p + len(old)
    out.append(text[last:])
    return ''.join(out), len(hits)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--selftest', action='store_true')
    sub = ap.add_subparsers(dest='cmd')
    c = sub.add_parser('census')
    c.add_argument('--src', default=os.path.normpath(os.path.join(HERE, '..', '..', 'psp', 'main_psp.c')))
    c.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')))
    c.add_argument('--cutlist', default=os.path.join(HERE, 'cutlist.txt'))
    c.add_argument('--json')
    c.add_argument('--tsv')
    c.add_argument('--basis')
    c.add_argument('--defs-from', help='a flavours.sh flavour dir: its build-manifest.json pspMakeFlags set the switches')
    r = sub.add_parser('rename')
    r.add_argument('old')
    r.add_argument('new')
    r.add_argument('--src', default=os.path.normpath(os.path.join(HERE, '..', '..', 'psp', 'main_psp.c')))
    r.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')),
                   help='the gpsp tree scanned for other TUs naming a non-static OLD')
    r.add_argument('-o', required=True)
    a = ap.parse_args(argv)
    if a.selftest:
        sys.path.insert(0, os.path.join(HERE, 'tests'))
        import importlib
        return importlib.import_module('source-split_test_globals').run()
    if a.cmd == 'census':
        cut = a.cutlist if a.cutlist and os.path.exists(a.cutlist) else None
        if FRAG_INCLUDE.search(open(a.src, encoding='utf-8', errors='replace').read()):
            print('globals census: REFUSED: %s includes fe/*.inc.c fragments (a split shell): the census '
                  'would see only the shell.  Run it on `split.py inverse` output.' % a.src, file=sys.stderr)
            return 2
        sha = src_sha(a.src)
        # the fixed modules.py ranges describe 01067e74 only; a cutlist only the source its header names
        if cut and cutlist_sha(cut) not in (sha, None):
            print('globals census: REFUSED: %s describes main_psp.c sha256 %s..., not this source (%s...): '
                  'pass the cutlist generated for this tree (split.py gen)' % (cut, (cutlist_sha(cut) or '?')[:12], sha[:12]),
                  file=sys.stderr)
            return 2
        if not cut and sha != SHA_01067E74:
            print('globals census: REFUSED: no --cutlist, and modules.py\'s fixed ranges are 01067e74\'s '
                  '(this source is sha256 %s...)' % sha[:12], file=sys.stderr)
            return 2
        rows, meta = census(a.src, a.root, cut, a.defs_from)
        rc = report(rows, meta, a.basis)
        if a.json:
            json.dump(rows, open(a.json, 'w'), indent=0)
        if a.tsv:
            with open(a.tsv, 'w', encoding='utf-8', newline='\n') as fo:
                fo.write('line\tname\tgroup\towner\tfragment\trel\tlife\tsection\tthreads\txmod_writers\text_users\n')
                for x in rows:
                    if x['extern']:
                        continue
                    fo.write('%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' % (
                        x['line'], x['name'], x['group'], x['owner'], x['fragment'], x['rel'], x['life'],
                        x['section'] or '', ','.join(x['threads']), ','.join(x['xmod_writers']), ','.join(x['ext_users'])))
        return rc
    if a.cmd == 'rename':
        text = open(a.src, encoding='utf-8', newline='').read()
        if os.path.abspath(a.o) == os.path.abspath(a.src):
            print('REFUSED: -o is the source file; rename writes a copy only', file=sys.stderr)
            return 1
        try:
            t2, n = rename(text, a.old, a.new, a.root)
        except Refused as e:
            print('REFUSED: ' + str(e), file=sys.stderr)
            return 1
        open(a.o, 'w', encoding='utf-8', newline='').write(t2)
        print('renamed %s -> %s at %d code token(s) -> %s' % (a.old, a.new, n, a.o))
        return 0
    ap.print_help()
    return 2


if __name__ == '__main__':
    sys.exit(main())
