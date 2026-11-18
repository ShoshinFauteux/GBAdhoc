#!/usr/bin/env python3
"""Static wiring checks for two small round-1 safety changes.

1. SPSC rings (transport_adhoc.c RX/TX/bulk, main_psp.c audio ring): every
   index publish or release (`A.x_head = head + 1`, `A.x_tail = tail + 1`,
   `ring_w++`, `ring_w += n`, `ring_r += frac >> 16`) is preceded by the
   compiler barrier, so a later edit cannot silently let GCC sink the slot
   stores (or hoist the slot loads) past the index store.  Flushes that
   only drop data (`ring_r = ring_w`, `A.bulk_tail = A.bulk_head`) and
   resets need none.

2. Mystery Gift game gate (main_psp.c mgift_start_io): mgift_game_ok(),
   compiled as written and asked about every 4-character code over A-Z0-9
   (and the empty one), accepts exactly the ROM codes mgift_cart.c serves a
   script for (read from the cart's text), and it is checked before
   mgnet_wifi_up() -- a non-Pokemon game must not wait up to 30 s for an
   association or write a network profile.

Each check runs against mutants too, so it is known to catch what it is
for."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import extract

ROOT = Path(__file__).resolve().parents[2]
ADHOC = ROOT / "netdrv" / "transport_adhoc.c"
MAIN = ROOT / "psp" / "main_psp.c"
CART = ROOT / "psp" / "mgift_cart.c"

PUBLISH = [
    (re.compile(r"^\s*A\.\w+_(head|tail) = (head|tail) \+ 1;"), "SPSC_BARRIER();"),
    (re.compile(r"^\s*ring_w(\+\+| \+= \w+);"), "RING_BARRIER();"),
    (re.compile(r"^\s*ring_r \+= frac >> 16;"), "RING_BARRIER();"),
]


# Every other write to a ring index must be one of these: flushes that only
# drop data and resets.  A write that is neither (a publish reworded as
# `ring_w = ring_w + n` or `A.rx_head = (head + 1) & M`) fails as
# unclassified until it is added to one list or the other.
WRITE = re.compile(r"^\s*(A\.\w+_(head|tail)\s*(=|\+=|-=|\+\+)|ring_[wr]\s*(=(?!=)|\+=|-=|\+\+|--)|(\+\+|--)\s*ring_[wr])")
NON_PUBLISH = [
    re.compile(r"^\s*A\.(\w+)_tail = A\.\1_head;"),
    re.compile(r"^\s*A\.\w+_head = A\.\w+_tail = 0;"),
    re.compile(r"^\s*ring_r = ring_w;"),
    re.compile(r"^\s*ring_w = ring_r = 0;"),
]
# Exact site counts: 6 transport publishes/releases (bulk, rx ring and tx,
# each a head and a tail) + 3 audio ring ones (producer, GB-link cushion,
# consumer).  A new site fails until it is classified here.
SITES = {"transport_adhoc.c": 6, "main_psp.c": 3}


def barrier_faults(text, name):
    """Publish lines whose previous code line is not the barrier, and ring
    index writes that are neither a known publish nor a known flush."""
    lines = text.splitlines()
    faults, seen = [], 0
    for i, ln in enumerate(lines):
        if WRITE.match(ln) and not any(rx.match(ln) for rx, _ in PUBLISH) \
                and not any(rx.match(ln) for rx in NON_PUBLISH):
            faults.append("%s:%d unclassified ring index write: %s" % (name, i + 1, ln.strip()))
        for rx, barrier in PUBLISH:
            if rx.match(ln):
                seen += 1
                j = i - 1
                while j >= 0 and (not lines[j].strip() or lines[j].strip().startswith(("/*", "*"))):
                    j -= 1
                if j < 0 or lines[j].strip() != barrier:
                    faults.append("no barrier before %s:%d %s" % (name, i + 1, ln.strip()))
    return seen, faults


def codes_served(cart):
    return set(re.findall(r'memcmp\(C\.recv_body \+ 92, "(\w{4})", 4\)', cart))


GATE_RUN = r'''
#include <stdio.h>
#include <string.h>
static char g_mg_game[5];
@FN@
int main(void)
{
   static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
   unsigned i, j, k, l, n = sizeof A - 1;
   g_mg_game[0] = 0;
   if (mgift_game_ok())
      puts("(empty)");
   for (i = 0; i < n; i++) for (j = 0; j < n; j++)
   for (k = 0; k < n; k++) for (l = 0; l < n; l++)
   {
      g_mg_game[0] = A[i]; g_mg_game[1] = A[j]; g_mg_game[2] = A[k]; g_mg_game[3] = A[l];
      g_mg_game[4] = 0;
      if (mgift_game_ok())
         puts(g_mg_game);
   }
   return 0;
}
'''


def gate_codes(main):
    """The codes mgift_game_ok() accepts, by running it (None: it would not build)."""
    fn = extract.extract(main, "fn:mgift_game_ok")
    with tempfile.TemporaryDirectory(prefix="gbadhoc-mggate-") as d:
        c, exe = Path(d) / "g.c", Path(d) / "g"
        c.write_text(GATE_RUN.replace("@FN@", fn))
        if subprocess.run(["gcc", "-std=gnu99", "-O2", "-Wall", "-Werror", str(c), "-o", str(exe)],
                          capture_output=True).returncode:
            return None
        return set(subprocess.run([str(exe)], capture_output=True, text=True,
                                  check=True).stdout.split())


def gate_before_wifi(main):
    main = extract.extract(main, "fn:mgift_start_io")
    m = re.search(r"static void mgift_start_io\(void\)\n\{(.*?)\n\}\n", main, re.S)
    if not m:
        return False
    body = m.group(1)
    g, w = body.find("if (!mgift_game_ok())"), body.find("mgnet_wifi_up(")
    return 0 <= g < w


def main():
    fails = 0
    # main_psp.c is scanned WHOLE (every ring index write must be classified):
    # extract.read_source() expands a split shell's fe/*.inc.c fragments in place
    adhoc, mainc, cart = ADHOC.read_text(), extract.read_source(str(MAIN)), CART.read_text()

    n1, f1 = barrier_faults(adhoc, "transport_adhoc.c")
    n2, f2 = barrier_faults(mainc, "main_psp.c")
    if n1 != SITES["transport_adhoc.c"] or n2 != SITES["main_psp.c"]:
        print("FAIL spsc: found %d + %d publish sites, expected exactly %d + %d" %
              (n1, n2, SITES["transport_adhoc.c"], SITES["main_psp.c"]))
        fails += 1
    for f in f1 + f2:
        print("FAIL spsc: %s" % f)
        fails += 1
    # mutant: drop one barrier, the check must see it
    mut = adhoc.replace("SPSC_BARRIER();\n            A.bulk_head = head + 1;",
                        "A.bulk_head = head + 1;", 1)
    if mut == adhoc or not barrier_faults(mut, "m")[1]:
        print("FAIL spsc: the dropped-barrier mutant was not caught")
        fails += 1
    # mutants: publishes reworded out of the known forms (critique-2)
    for a, b in (("ring_w += n;", "ring_w = ring_w + n;"),
                 ("A.tx_head = head + 1;", "A.tx_head = (head + 1) & 63;")):
        if a.startswith("ring"):
            # the GB-link cushion's publish (gblink_audio_session), by its marker
            src = mainc
            site = extract.extract(mainc, "spsc:gblink_publish")
            mut = src.replace(site, site.replace(a, b, 1), 1)
        else:
            src = adhoc
            mut = src.replace(a, b, 1)
        if mut == src or not barrier_faults(mut, "m")[1]:
            print("FAIL spsc: the reworded-publish mutant %r was not caught" % b)
            fails += 1
    if not fails:
        print("PASS spsc: %d publish/release sites, each behind its barrier" % (n1 + n2))

    served, gated = codes_served(cart), gate_codes(mainc)
    if not served or gated is None:
        print("FAIL mgift: could not parse the served codes (%r) or the gate (%r)" % (served, gated))
        fails += 1
    elif served != gated:
        print("FAIL mgift: the gate accepts %s, the cart serves %s" % (sorted(gated), sorted(served)))
        fails += 1
    else:
        print("PASS mgift: gate accepts exactly %s" % ", ".join(sorted(gated)))
    if not gate_before_wifi(mainc):
        print("FAIL mgift: mgift_game_ok() is not checked before mgnet_wifi_up()")
        fails += 1
    else:
        print("PASS mgift: refused before Wi-Fi comes up")
    # mutants: an extra code, a missing code, a prefix match, the check moved
    # after Wi-Fi -- each must be caught; and the same gate written as a table
    # (the codes unchanged) must still pass: the run, not the spelling, decides
    gate = extract.extract(mainc, "fn:mgift_game_ok")
    body = gate[gate.index("{"):gate.rindex("}") + 1]
    table = ('{\n   static const char *const k[] = { "BPEE", "BPRE", "BPGE" };\n   unsigned i;\n'
             '   for (i = 0; i < 3; i++)\n      if (!memcmp(g_mg_game, k[i], 5))\n'
             '         return 1;\n   return 0;\n}')
    m1 = mainc.replace('!strcmp(g_mg_game, "BPEE")', '!strcmp(g_mg_game, "BPEE") || !strcmp(g_mg_game, "BPEJ")', 1)
    m2 = mainc.replace('!strcmp(g_mg_game, "BPGE") ||', '', 1)
    m4 = mainc.replace(body, '{\n   return !strncmp(g_mg_game, "BP", 2);\n}', 1)
    m3 = mainc.replace("if (!mgift_game_ok())", "if (0)", 1)
    if (any(x == mainc for x in (m1, m2, m3, m4)) or gate_codes(m1) == served or
            gate_codes(m2) == served or gate_codes(m4) == served or gate_before_wifi(m3)):
        print("FAIL mgift: a mutant was not caught")
        fails += 1
    if gate_codes(mainc.replace(body, table, 1)) != served:
        print("FAIL mgift: the same gate written as a table was refused")
        fails += 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
