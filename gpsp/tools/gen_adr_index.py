#!/usr/bin/env python3
"""Generate docs/ADR-INDEX.md from the actual headings in docs/DECISIONS.md.

Generated rather than hand-written so the identifiers, titles and line numbers
cannot drift from the file they describe, and so it can be regenerated after a
reviewed migration.

STATUS IS NOT INVENTED.  Only relationships stated in a heading (or verified
elsewhere against code and hardware evidence) are recorded.  Everything else is
`unreviewed`, which means nobody has checked it against current behaviour -- not
that it is current.
"""
import io
import re
from collections import defaultdict

SRC = 'docs/DECISIONS.md'
OUT = 'docs/ADR-INDEX.md'

# Series labels for the two numbering lines that collide.  Aliases, NOT a
# renumbering: renumbering silently breaks every existing reference.
PHASE6_FROM_LINE = 3200          # the imported phase6-coreopt series starts here

# Verified relationships and outcomes.  Each is either stated in a heading in
# DECISIONS.md or established by evidence recorded in this branch.
KNOWN = {
    '0010': ('superseded', 'ADR-0017', 'netdrv ARQ timing'),
    '0018': ('superseded', 'ADR-0019', 'frameskip policy'),
    '0040': ('superseded', 'ADR-0049', 'GE / presentation'),
    '0050': ('corrected-by-addendum', 'ADR-0050 addendum',
             'netdrv transmit queue'),
    '0042': ('corrected-by-addendum', 'ADR-0042 addendum', 'RFU receive cap'),
}
# Subsystem guesses are omitted rather than guessed; only fill where obvious from
# the title's own words.
SUBSYS = [
    (r'netdrv|ARQ|RTO|transport|transmit queue|ad-hoc|RFU|peer', 'networking'),
    (r'renderer|blit|palette|VRAM|GE|overdraw|oracle|frame-exact', 'video'),
    (r'SMC|dynarec|JIT|flush', 'dynarec'),
    (r'\.sav|SRAM|save', 'storage'),
    (r'autopilot|harness|rig|unattended', 'harness'),
    (r'frameskip|pace|frame rate|vblank|fps', 'pacing'),
    (r'memory posture|heap|PSP_LARGE_MEMORY', 'memory'),
    (r'UI|clock|suspend', 'frontend'),
]

H = re.compile(r'^#+\s*(ADR-(\d{4}))(\s+addendum\s*\d*)?\s*[—-]+\s*(.*)$')


def subsystem(title):
    for pat, name in SUBSYS:
        if re.search(pat, title, re.I):
            return name
    return '-'


def main():
    lines = io.open(SRC, encoding='utf-8', newline='').read().split('\n')
    recs = defaultdict(list)
    for i, l in enumerate(lines):
        m = H.match(l)
        if m:
            recs[m.group(2)].append(
                {'line': i + 1, 'addendum': bool(m.group(3)),
                 'title': m.group(4).strip()})

    collisions = {n for n, rs in recs.items()
                  if len([r for r in rs if not r['addendum']]) > 1}
    present = sorted(int(n) for n in recs)
    gaps = [n for n in range(1, max(present) + 1) if n not in present]

    o = []
    w = o.append
    w('# Canonical ADR status index')
    w('')
    w('**Generated** from the headings in `%s` by' % SRC)
    w('`gen_adr_index.py`. Regenerate it rather than editing it by hand, so the')
    w('identifiers and line numbers cannot drift from the file they describe.')
    w('')
    w('`status` is deliberately conservative. **`unreviewed` means nobody has')
    w('checked the record against current behaviour — not that it is current.**')
    w('Only relationships stated in a heading, or established by evidence')
    w('recorded in this branch, are marked otherwise.')
    w('')
    w('## How to read the collisions')
    w('')
    w('Five identifiers carry two *primary* records each:')
    w('')
    w('    %s' % '  '.join('ADR-%s' % n for n in sorted(collisions)))
    w('')
    w('`ADR-0008` is a plain duplicate-numbering mistake: two unrelated mainline')
    w('records share it. `ADR-0033`..`ADR-0036` are an imported **phase6-coreopt**')
    w('series overlapping the **mainline** numbering.')
    w('')
    w('They are disambiguated here by the aliases `(mainline)` and `(phase6)`,')
    w('and by line number. **They are not renumbered.** Active source comments')
    w('cite these numbers, and renumbering would silently break every reference')
    w('while leaving the citations looking correct.')
    w('')
    w('A heading of the form `ADR-NNNN addendum` is **not** a collision — it is a')
    w('continuation of the same record, and often the place where that record\'s')
    w('original conclusion was corrected. An earlier count of "eight duplicated')
    w('identifiers" came from a naive grep that treated addenda as collisions;')
    w('the real figure is five.')
    w('')
    w('## Coverage boundary')
    w('')
    w('This file stops at **ADR-%04d**. Active source and the outer HANDOVER cite'
      % max(present))
    w('ADR-0054..ADR-0082, which are **not recorded here at all**. Do not assume a')
    w('number above %04d is missing or invalid; look in the outer handover'
      % max(present))
    w('document. Those records must be checked against current code and hardware')
    w('evidence before being imported, one at a time.')
    w('')
    if gaps:
        w('Numbers absent from this file below that ceiling: %s.'
          % ', '.join('ADR-%04d' % n for n in gaps))
        w('')
    w('One known discrepancy, unresolved: the harness control-channel rename')
    w('(`autopilot.ini` -> `.gpsp-harness.ini`) is recorded here as **ADR-0036')
    w('(mainline)**, but active source and tooling cite it as **ADR-0067**. Both')
    w('names are in use. Do not "fix" either until the outer records are')
    w('reconciled.')
    w('')
    w('## Records')
    w('')
    w('| record | alias | line | status | replaced by | subsystem | title |')
    w('| --- | --- | --- | --- | --- | --- | --- |')
    for num in sorted(recs):
        for r in sorted(recs[num], key=lambda x: x['line']):
            ident = 'ADR-%s' % num
            if r['addendum']:
                ident += ' addendum'
            alias = '-'
            if num in collisions and not r['addendum']:
                alias = 'phase6' if r['line'] >= PHASE6_FROM_LINE else 'mainline'
            status, repl = 'unreviewed', '-'
            if r['addendum']:
                status = 'addendum'
            elif num in KNOWN:
                status, repl, _ = KNOWN[num]
            title = r['title'].replace('|', '\\|')
            w('| `%s` | %s | %d | %s | %s | %s | %s |'
              % (ident, alias, r['line'], status, repl,
                 subsystem(r['title']), title))
    w('')
    w('## What still has to happen')
    w('')
    w('1. Review each `unreviewed` row against current code, and set it to')
    w('   `current`, `superseded`, `withdrawn`, `rejected` or `experimental`.')
    w('2. Import ADR-0054..0082 from the outer HANDOVER one at a time, checking')
    w('   each claim before it is recorded as a decision.')
    w('3. Resolve the ADR-0036/ADR-0067 naming of the control-channel rename.')
    w('4. Only then consider a reviewed migration that gives the colliding')
    w('   records unique identifiers, updating every citing source comment in the')
    w('   same change.')
    io.open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(o) + '\n')
    print('wrote %s: %d numbers, %d headings, %d collisions'
          % (OUT, len(recs), sum(len(v) for v in recs.values()),
             len(collisions)))


if __name__ == '__main__':
    main()
