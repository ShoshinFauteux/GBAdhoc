#!/usr/bin/env python3
"""Static wiring checks for two small round-1 safety changes.

1. SPSC rings (transport_adhoc.c RX/TX/bulk, main_psp.c audio ring): every
   index publish or release (`A.x_head = head + 1`, `A.x_tail = tail + 1`,
   `ring_w++`, `ring_w += n`, `ring_r += frac >> 16`) is preceded by the
   compiler barrier, so a later edit cannot silently let GCC sink the slot
   stores (or hoist the slot loads) past the index store.  Flushes that
   only drop data (`ring_r = ring_w`, `A.bulk_tail = A.bulk_head`) and
   resets need none.

2. Mystery Gift game gate (main_psp.c mgift_start_io): mgift_game_ok()
   accepts exactly the ROM codes mgift_cart.c serves a script for, and it
   is checked before mgnet_wifi_up() -- a non-Pokemon game must not wait
   up to 30 s for an association or write a network profile.

Each check runs against mutants too, so it is known to catch what it is
for."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ADHOC = ROOT / "netdrv" / "transport_adhoc.c"
MAIN = ROOT / "psp" / "main_psp.c"
CART = ROOT / "psp" / "mgift_cart.c"

PUBLISH = [
    (re.compile(r"^\s*A\.\w+_(head|tail) = (head|tail) \+ 1;"), "SPSC_BARRIER();"),
    (re.compile(r"^\s*ring_w(\+\+| \+= \w+);"), "RING_BARRIER();"),
    (re.compile(r"^\s*ring_r \+= frac >> 16;"), "RING_BARRIER();"),
]


def barrier_faults(text, name):
    """Publish lines whose previous code line is not the barrier."""
    lines = text.splitlines()
    faults, seen = [], 0
    for i, ln in enumerate(lines):
        for rx, barrier in PUBLISH:
            if rx.match(ln):
                seen += 1
                j = i - 1
                while j >= 0 and (not lines[j].strip() or lines[j].strip().startswith(("/*", "*"))):
                    j -= 1
                if j < 0 or lines[j].strip() != barrier:
                    faults.append("%s:%d %s" % (name, i + 1, ln.strip()))
    return seen, faults


def codes_served(cart):
    return set(re.findall(r'memcmp\(C\.recv_body \+ 92, "(\w{4})", 4\)', cart))


def gate_codes(main):
    m = re.search(r"static int mgift_game_ok\(void\)\n\{(.*?)\n\}", main, re.S)
    if not m:
        return None
    return set(re.findall(r'!strcmp\(g_mg_game, "(\w{4})"\)', m.group(1)))


def gate_before_wifi(main):
    m = re.search(r"static void mgift_start_io\(void\)\n\{(.*?)\n\}\n", main, re.S)
    if not m:
        return False
    body = m.group(1)
    g, w = body.find("if (!mgift_game_ok())"), body.find("mgnet_wifi_up(")
    return 0 <= g < w


def main():
    fails = 0
    adhoc, mainc, cart = ADHOC.read_text(), MAIN.read_text(errors="replace"), CART.read_text()

    n1, f1 = barrier_faults(adhoc, "transport_adhoc.c")
    n2, f2 = barrier_faults(mainc, "main_psp.c")
    # 5 transport publishes/releases + 3 audio ring ones (producer, cushion, consumer)
    if n1 < 5 or n2 < 3:
        print("FAIL spsc: found %d + %d publish sites, expected >= 5 + 3" % (n1, n2))
        fails += 1
    for f in f1 + f2:
        print("FAIL spsc: no barrier before %s" % f)
        fails += 1
    # mutant: drop one barrier, the check must see it
    mut = adhoc.replace("SPSC_BARRIER();\n            A.bulk_head = head + 1;",
                        "A.bulk_head = head + 1;", 1)
    if mut == adhoc or not barrier_faults(mut, "m")[1]:
        print("FAIL spsc: the dropped-barrier mutant was not caught")
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
    # mutants: an extra code, a missing code, the check moved after Wi-Fi
    m1 = mainc.replace('!strcmp(g_mg_game, "BPEE")', '!strcmp(g_mg_game, "BPEE") || !strcmp(g_mg_game, "BPEJ")', 1)
    m2 = mainc.replace('!strcmp(g_mg_game, "BPGE") ||', '', 1)
    m3 = mainc.replace("if (!mgift_game_ok())", "if (0)", 1)
    if gate_codes(m1) == served or gate_codes(m2) == served or gate_before_wifi(m3):
        print("FAIL mgift: a mutant was not caught")
        fails += 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
