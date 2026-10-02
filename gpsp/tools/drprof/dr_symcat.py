#!/usr/bin/env python3
"""dr_symcat.py BIN.nm BIN.members > symcat.txt

Assign every code symbol of the twin binary to one of the plugin's coarse
categories (drprof_plugin.c CAT_*).  Output lines: "addr size cat name".

BIN.nm      `nm -S --defined-only dr_host_X`       (addresses + sizes)
BIN.members `nm -A --defined-only libcore_X.a`     (which core object defines
            each symbol -- the category is mostly decided by the SOURCE FILE,
            so a C++-mangled renderer symbol needs no name rule)

The rules are deliberately few and are printed by --explain; everything not
matched falls to `core` (inside the GBA core) or `libc`/`host` (outside it).
"""
import re
import sys

CATS = ["jit", "jmem", "stub", "memc", "disp", "jdisp", "cyc", "upd", "xlat",
        "flush", "video", "sound", "core", "libc", "host", "other"]
C = {n: i for i, n in enumerate(CATS)}

MEMC_NAMES = re.compile(r"^(read_memory|write_memory|write_io_register|"
                        r"read_io_register|load_gamepak_page|read_backup|"
                        r"write_backup|read_eeprom|write_eeprom|write_gpio|"
                        r"read_gpio|write_rtc|read_rtc|function_cc_read|"
                        r"read_open|write_flash|read_flash|write_sram|read_sram)")


def cat_for(name, member):
    m = member or ""
    if m.endswith("video.o") or m.endswith("gba_cc_lut.o"):
        return "video"
    if m.endswith("sound.o"):
        return "sound"
    if m.endswith("mips_stub.o"):
        if name == "mips_update_gba" or name == "execute_arm_translate_internal":
            return "upd"
        if name.startswith("mips_indirect_branch"):
            return "disp"
        if name == "write_io_epilogue":
            return "memc"
        if name in ("lookup_pc", "lookup_pc_noflags"):
            return "disp"
        if name in ("return_to_main", "cpu_sleep_loop"):
            return "upd"
        if name == "smc_write":
            return "flush"
        return "core"
    if m.endswith("cpu_threaded.o"):
        if name.startswith("block_lookup_"):
            return "disp"
        if (name.startswith("flush_") or name.startswith("smc_") or
                name.startswith("ramtag_") or name.startswith("drprof_forget")):
            return "flush"
        if name in ("drprof_zone",):
            return "host"
        if (name.startswith("translate_") or name.startswith("scan_") or
                name.startswith("drprof_") or name.startswith("init_emitter") or
                name.startswith("emit_") or name.startswith("allocate_tag") or
                name.startswith("init_bios_hooks") or "flag_eliminate" in name):
            return "xlat"
        if name == "execute_arm_translate":
            return "upd"
        return "core"
    if m.endswith("main.o"):
        if name == "update_gba":
            return "upd"
        return "core"
    if m.endswith("gba_memory.o"):
        if "dma" in name:
            return "core"
        if MEMC_NAMES.match(name):
            return "memc"
        return "memc" if name.startswith(("read_", "write_")) else "core"
    if m.endswith("libretro.o") or "/libretro-common/" in m or m.startswith("compat"):
        return "host"
    return "core"


HOST_PREFIX = ("fe_", "netdrv", "netpacket", "gbcore", "tgbx_", "gb_", "drprof_host",
               "main", "a_frames", "v_frame", "no_input", "fnv32w", "mark",
               "lz4", "evt_", "ap_")


def main():
    nm_path, mem_path = sys.argv[1], sys.argv[2]
    member = {}
    for ln in open(mem_path, errors="replace"):
        # libcore_X.a:video.o:00001234 T name
        parts = ln.rstrip("\n").split(":", 2)
        if len(parts) < 3:
            continue
        rest = parts[2].split()
        if len(rest) >= 3 and rest[1] in "TtWw":
            member[rest[2]] = parts[1]
    out = []
    for ln in open(nm_path, errors="replace"):
        f = ln.split()
        if len(f) == 4:
            addr, size, typ, name = f
        elif len(f) == 3:
            addr, typ, name = f
            size = "0"
        else:
            continue
        if name in ("rom_translation_cache", "ram_translation_cache",
                    "rom_translation_cache_static", "ram_translation_cache_static"):
            out.append((int(addr, 16), int(size, 16), C["other"], name))
            continue
        if typ not in "TtWw":
            continue
        if name.startswith(".pic."):
            name = name[5:]
        if name in member:
            c = cat_for(name, member[name])
        elif name.startswith(HOST_PREFIX):
            c = "host"
        else:
            c = "libc"
        out.append((int(addr, 16), int(size, 16), C[c], name))
    out.sort()
    # asm labels (mips_stub.S defsymbl, local labels) carry no .size: extend
    # them to the next symbol so every instruction of a stub is attributed
    for i, (a, s, c, n) in enumerate(out):
        if s == 0 and i + 1 < len(out) and n not in (
                "rom_translation_cache", "ram_translation_cache"):
            nxt = out[i + 1][0]
            if nxt > a:
                out[i] = (a, min(nxt - a, 0x4000), c, n)
    for a, s, c, n in out:
        print("%08x %x %d %s" % (a, s, c, n))


if __name__ == "__main__":
    main()
