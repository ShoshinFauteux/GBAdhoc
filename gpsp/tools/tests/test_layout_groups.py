#!/usr/bin/env python3
"""test_layout_groups.py -- tools/layout/groups_ld.sh and psp/Makefile's layout
groups (code-health C3-2; docs/LAYOUT-PINNING.md "Object groups").

Links small host objects with host gcc/ld through a script shaped like
psp/gbadhoc.ld's .text (INCLUDE groups.ld, then the default's generic
statement as the catch-all):

* grouped in command-line order, the text is byte-identical to the default
  statement's, and every symbol is where the default put it;
* an object in no group fails the link with the ASSERT text (the negative
  test C-3.md asks for);
* `:FILE` is exact: `me_host.o`'s group does not take `home_host.o` (a plain
  `*me_host.o` would), and an archive member named like a grouped object
  (`a.o` inside libq.a) stays in the catch-all;
* psp/Makefile builds OBJS only from the groups (no `OBJS +=` / `OBJS :=`);
* the pins (C3-3): through tools/layout/pins.awk and `groups_ld.sh --pins`, a
  group starts at its pin's address; a pin the code before it outgrew fails a
  strict link naming the pin and wraps 8 KiB in a non-strict one; a lock
  section without its five pins stops pins.awk.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GEN = os.path.join(ROOT, "tools", "layout", "groups_ld.sh")
MAKEFILE = os.path.join(ROOT, "psp", "Makefile")

fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1


SRC = {
    "head.c": "int h1(void) { return 1; }\n",
    "a.c": "int a1(void) { return 2; }\nint a2(void) { return 3; }\n",
    "b.c": "int b1(void) { return 4; }\n",
    "c.c": "extern int q1(void);\nint c1(void) { return q1() + 5; }\n",
    "me_host.c": "int m1(void) { return 6; }\n",
    "home_host.c": "int o1(void) { return 7; }\n",
}
# the archive member: named like a grouped object, defines what c.o calls
MEMBER = "int q1(void) { return 8; }\nint q2(void) { return 9; }\n"
OBJS = ["head.o", "a.o", "b.o", "c.o", "home_host.o", "me_host.o"]

SCRIPT = """SECTIONS
{
  .text :
  {
%s
    *(.text .stub .text.* .gnu.linkonce.t.*)
  }
  /DISCARD/ : { *(.note.GNU-stack) *(.comment) *(.eh_frame) *(.note.gnu.property) }
}
"""


def sh(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, universal_newlines=True)


def link(tmp, groups, out):
    """-> (rc, ld output, text bytes, {symbol: address})"""
    if groups is None:
        inc = ""
    else:
        g = sh(["sh", GEN] + groups, tmp)
        if g.returncode != 0:
            return g.returncode, g.stdout, None, {}
        with open(os.path.join(tmp, "groups.ld"), "w") as f:
            f.write(g.stdout)
        inc = "    INCLUDE groups.ld"
    with open(os.path.join(tmp, "t.ld"), "w") as f:
        f.write(SCRIPT % inc)
    r = sh(["ld", "-T", "t.ld", "-e", "h1", "-o", out] + OBJS + ["libq.a"], tmp)
    if r.returncode != 0:
        return r.returncode, r.stdout, None, {}
    sh(["objcopy", "-O", "binary", "-j", ".text", out, out + ".bin"], tmp)
    text = open(os.path.join(tmp, out + ".bin"), "rb").read()
    syms = {}
    for ln in sh(["nm", out], tmp).stdout.splitlines():
        p = ln.split()
        # __layout_ungrouped is the check's own mark, not code
        if len(p) == 3 and p[1] in "Tt" and not p[2].startswith("__layout"):
            syms[p[2]] = int(p[0], 16)
    return 0, r.stdout, text, syms


PINS_AWK = os.path.join(ROOT, "tools", "layout", "pins.awk")
PIN_SCRIPT = """INCLUDE pins.ld
SECTIONS
{
  . = 0x08804040;
  .text :
  {
    INCLUDE groups.ld
    *(.text .stub .text.* .gnu.linkonce.t.*)
  }
  /DISCARD/ : { *(.note.GNU-stack) *(.comment) *(.eh_frame) *(.note.gnu.property) }
}
"""
PIN_GROUPS = ["head:head.o", "fe_head:a.o b.o", "fe_common:c.o",
              "fe_tail:home_host.o me_host.o"]


def pin_link(tmp, fe_common, core, strict, sweep=None):
    """pins.ld from a lock through tools/layout/pins.awk, groups.ld with pins
    in front of fe_common and the catch-all -> (rc, output, {symbol: addr})."""
    with open(os.path.join(tmp, "p.lock"), "w") as f:
        f.write("[t]\npin main 0x08804040\npin fe_head 0x08804040\n"
                "pin fe_common 0x%08x\npin fe_tail 0x08804040\npin core 0x%08x\n"
                % (fe_common, core))
    awk = ["awk", "-v", "flavour=t", "-v", "strict=%d" % strict]
    if sweep is not None:
        awk += ["-v", "sweep=%s" % sweep]
    r = sh(awk + ["-f", PINS_AWK, "p.lock"], tmp)
    if r.returncode != 0:
        return r.returncode, r.stdout, {}
    open(os.path.join(tmp, "pins.ld"), "w").write(r.stdout)
    g = sh(["sh", GEN, "--pins", "fe_common core"] + PIN_GROUPS, tmp)
    open(os.path.join(tmp, "groups.ld"), "w").write(g.stdout)
    open(os.path.join(tmp, "p.ld"), "w").write(PIN_SCRIPT)
    r = sh(["ld", "-T", "p.ld", "-e", "h1", "-o", "p.elf"] + OBJS + ["libq.a"], tmp)
    syms = {}
    if r.returncode == 0:
        for ln in sh(["nm", "p.elf"], tmp).stdout.splitlines():
            p = ln.split()
            if len(p) == 3:
                syms[p[2]] = int(p[0], 16)
    return r.returncode, r.stdout, syms


def pin_tests(tmp):
    """The pins (code-health C3-3): a group starts at its pin's address; a
    pin the code before it outgrew fails a strict link with the pin's name,
    and in a non-strict link wraps a whole 8 KiB up; a lock without the five
    pins stops pins.awk."""
    rc, out, syms = pin_link(tmp, 0x08805a40, 0x08806010, 1)
    check(rc == 0 and syms.get("c1") == 0x08805a40 and syms.get("__pin_at_fe_common") == 0x08805a40,
          "pin fe_common: c.o starts at the pinned address (%s)" % out.strip()[:160])
    check(syms.get("q1") == 0x08806010 and syms.get("layout_pad_core") is not None,
          "pin core: the catch-all starts at its pin; the old pad name marks the gap")
    rc, out, syms = pin_link(tmp, 0x08804048, 0x08806010, 1)
    check(rc != 0 and "layout pin fe_common" in out,
          "strict: a pin the code before it grew past fails the link, naming the pin")
    rc, out, syms = pin_link(tmp, 0x08804048, 0x08808010, 0)
    check(rc == 0 and syms.get("c1") == 0x08804048 + 8192,
          "not strict: the same pin wraps one 8 KiB page up")
    # the layout sweep (LAYOUT_PAD_TEXT, pins.awk sweep=N): the pins move N,
    # so the code after them moves N mod 8 KiB; 0 is the unswept link
    rc, out, base = pin_link(tmp, 0x08805a40, 0x08806010, 1, sweep="0")
    rc1, out1, sw = pin_link(tmp, 0x08805a40, 0x08806010, 0, sweep="1024")
    check(rc == 0 and rc1 == 0 and base.get("c1") == 0x08805a40
          and sw.get("c1") == 0x08805a40 + 1024 and sw.get("q1") == 0x08806010 + 1024,
          "sweep=1024: fe_common and core move 1024 B (the sweep is not absorbed by the pins)")
    pl = open(os.path.join(tmp, "pins.ld")).read()
    check("__pin_main = 0x08804040;" in pl and "__pin_fe_common = 0x08805a40 + 1024;" in pl,
          "sweep: main's pin stays, the frontend and core pins move")
    rc, out, sw = pin_link(tmp, 0x08805a40, 0x08806010, 0, sweep="9216")
    check(rc == 0 and (sw.get("c1", 0) - 0x08805a40) % 8192 == 1024,
          "sweep=9216: the same sets as 1024 (mod 8 KiB)")
    rc, out, sw = pin_link(tmp, 0x08805a40, 0x08806010, 0, sweep="x")
    check(rc != 0 and "sweep=x" in out, "pins.awk refuses a sweep that is not a number")
    with open(os.path.join(tmp, "p.lock"), "w") as f:
        f.write("[t]\npin main 0x08804040\n")
    r = sh(["awk", "-v", "flavour=t", "-v", "strict=1", "-f", PINS_AWK, "p.lock"], tmp)
    check(r.returncode != 0 and "no `pin fe_head`" in r.stdout,
          "pins.awk: a flavour missing a pin stops the make")


def main():
    for tool in ("gcc", "ld", "ar", "objcopy", "nm", "sh"):
        if not shutil.which(tool):
            print("SKIP: %s not found (host gcc/binutils needed)" % tool)
            return 0
    tmp = tempfile.mkdtemp(prefix="layout_groups_")
    try:
        for name, src in SRC.items():
            with open(os.path.join(tmp, name), "w") as f:
                f.write(src)
            r = sh(["gcc", "-c", "-O1", "-ffunction-sections",
                    "-fno-asynchronous-unwind-tables", name], tmp)
            if r.returncode != 0:
                print(r.stdout)
                check(False, "compile %s" % name)
                return 1
        os.mkdir(os.path.join(tmp, "lib"))
        with open(os.path.join(tmp, "lib", "a.c"), "w") as f:
            f.write(MEMBER)
        sh(["gcc", "-c", "-O1", "-ffunction-sections",
            "-fno-asynchronous-unwind-tables", "-o", "lib/a.o", "lib/a.c"], tmp)
        # the member is called a.o inside the archive, like the grouped a.o
        sh(["ar", "rcs", "../libq.a", "a.o"], os.path.join(tmp, "lib"))

        rc0, out0, ref, ref_syms = link(tmp, None, "ref.elf")
        check(rc0 == 0 and ref, "the default statement alone links (%s)" % out0.strip()[:120])

        full = ["head:head.o", "g1:a.o b.o", "g2:c.o", "g3:home_host.o me_host.o"]
        rc, out, text, syms = link(tmp, full, "g.elf")
        check(rc == 0, "every object grouped: the link passes (%s)" % out.strip()[:200])
        check(text is not None and text == ref, "grouped in command-line order: .text byte-identical to the default's")
        check(syms == ref_syms and "q1" in syms,
              "every function where the default put it (the archive's q1 included)")
        if syms and "q1" in syms and "o1" in syms:
            check(syms["q1"] > max(syms["o1"], syms["m1"]),
                  "the archive member a.o stays in the catch-all, after every group")

        rc, out, text, syms = link(tmp, ["head:head.o", "g1:a.o", "g2:c.o",
                                         "g3:home_host.o me_host.o"], "n.elf")
        check(rc != 0 and "layout groups:" in out,
              "b.o in no group: the link fails with the ASSERT text")

        rc, out, text, syms = link(tmp, ["head:head.o", "g1:a.o b.o", "g2:c.o",
                                         "g3:me_host.o"], "s.elf")
        check(rc != 0 and "layout groups:" in out,
              "`:me_host.o` does not take home_host.o (home_host.o in no group fails)")

        pin_tests(tmp)

        open(os.path.join(tmp, "lib", "crt0.o"), "w").close()
        g = sh(["sh", GEN, "x:*/crt0.o b.o"], tmp).stdout
        check(":*/crt0.o(" in g and ":b.o(" in g and "lib/crt0.o" not in g,
              "a wildcard FILE reaches ld as written (the shell does not glob it to lib/crt0.o)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    mk = open(MAKEFILE, "rb").read().decode("latin-1")
    bad = [l for l in mk.splitlines() if re.match(r"^OBJS\s*(\+=|:=)", l)]
    check(not bad, "psp/Makefile adds to OBJS only through a group (%s)" % bad)
    comp = re.findall(r"^OBJS\s*=\s*(.*)$", mk, re.M)
    check(comp == ["$(HEAD_OBJS) $(FE_HEAD_OBJS) $(FE_COMMON_OBJS) $(GB_OBJS) $(FE_TAIL_OBJS)"],
          "OBJS is the groups joined, in link order")
    print("%d failure(s)" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
