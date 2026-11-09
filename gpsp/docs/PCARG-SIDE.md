# PCARG_SIDE: the PC argument leaves the call site

Branch `claude/pcarg-side` (base claude/beta5-base e70d60aa). Switch `PCARG_SIDE` (gpsp_features.h, on;
`PCARG_SIDE=0` builds it off). Scripts and twin data: `builds/pcarg-out/`.

## 1. Why

The PSP's translated code is I-cache bound (16 KiB, 2-way, 64 B lines): codegen ideas that add hot bytes lost
4-12 % on hardware even with fewer instructions, and only ideas that shrink hot code pay. In the hot translated
bytes of the shooters, the largest removable item is the PC argument of a memory call site
(claude/cache-aware-codegen, docs/CACHE-AWARE-CODEGEN.md section 4: memory call sites are 36 % of Doom's hot
bytes, the PC argument 8 %):

    load:   jal stub ; addiu a1, reg_pc, imm          store:  addiu a2, reg_pc, imm ; mov a1 ; jal stub (mov a1 in the slot)

`imm` is the site's PC delta, and since the MonoMyth fix it also carries `k`, the cycles `reg_cycles` runs
ahead of the access (iok_encode). Only the stubs' slow paths read it: open bus and BIOS protection (openload),
the live timer counters (I/O loads of TM0..3), I/O writes and `smc_write`. The fast paths never look.

An older version (codegen-shrink a7f2ddcd, loads only, no `k`) kept its PC table as a trailer INSIDE the
translation cache: hardware -0.2..-1.8 % on 8 of 9 games, but Hot Wheels +2.1 % because the trailers made RAM
code bigger (RAM translation flushes 229 -> 276).

## 2. Design

* **New-form site.** Load: `jal stub ; X`, where X is the access's last address word (computed by this access,
  not a branch and not in a delay slot), swapped into the delay slot. Store: `jal stub ; mov a1` (the value
  word keeps the slot; ARM and Thumb). One word less per site. Swap, LDM/STM and every other a1/a2 user stay
  as they were.
* **Side table, outside the translation caches.** Per cache (ROM, RAM): `key` = the site's return address
  (jal + 8), `imm` = the immediate the addiu would have carried, so the argument is exactly `reg_pc + imm`,
  `k` included. Entries are kept sorted by address: a block's sites are appended when the block is committed
  (code is only written at a cache's translation_ptr), and a block that starts below the end of the last one
  (the pointer moved back: a flush, or an abandoned translation) first drops every entry at or after it.
  One 128 KiB object per core instance (`pcarg_st`): ROM 12,288 entries, RAM 8,192, a 512-entry front cache.
  A full table, a block with 256 new-form sites, or a word that may not move makes the site OLD form
  (unchanged code); the stubs handle both, so the fallback is exact.
* **Recovery.** `mips_stub.S pcarg_dec`: return address -> front cache (direct mapped) -> on a miss
  `pcarg_lookup` (binary search, fills the front cache). Found: argument = `reg_pc + imm`; not found: the
  site is old form and its register holds the argument. Then the same decode as `emit_iok_decode`.
* **Stubs.** The openload and I/O-write decodes and the timer-read path call pcarg_dec after their register
  save; `smc_write` too. The BIOS stub's inline "PC in BIOS?" test needs the PC, so unaligned BIOS reads go
  to the openload C path, which applies the same protection (`read_memory*`). Every rewritten slow path is
  padded to its old size (`pcarg_pad_to`): no stub moves, so the hot stubs keep their I-cache sets, and
  `mips_stub.o(.text)` (the head of the hot order) keeps its size (`pcarg_dec` has its own section).
* **Determinism.** The table's content is a function of the translation history alone; the front cache only
  changes how fast an answer comes. The JIT code's guest effect is identical by construction (section 3).

## 3. Exactness: the pad oracle

`PCARG_SIDE_PAD=1` (twin only) emits a nop after every new-form site, so every block keeps the base's size,
every flush happens at the same block, and the guest must run exactly as the base. Twins of the cf44b8a9e624
manifest (e70d60aa) vs the same plus PCARG_SIDE_PAD, 3,900 frames (the Lab window): **state hashes identical
on Doom II, Duke, Ice Nine, Top Gear Rally, Hot Wheels, Emerald battle and FireRed** (Emerald with 6,056
sites past the ROM cap, so both forms mixed). The real arm (no pad) is identical on Emerald and FireRed and
diverges by timing on the others (smaller RAM code moves a full flush; first difference at frame 179 Hot
Wheels .. 3,667 Doom): Hot Wheels' cache resets 286 -> 237 in the window.

## 4. Twin prediction (cache-aware twin, docs/CACHE-AWARE-CODEGEN.md)

Arms: twin of cf44b8a9e624's manifest (e70d60aa) vs the same + PCARG_SIDE; placement from the cf44 lab ELF;
frames 300..3900 (Ice Nine and Emerald end with their scripts); 3.6 ns/instruction + 215 ns/miss;
% of hardware work ms.

| fixture | instr/frame | misses/frame | pred. pinned base | mean of 4 bases (range) |
|---|---:|---:|---:|---:|
| Doom II | -2.67 % | -1,819 | -3.30 % | -2.92 % (-3.54 .. -1.97) |
| Duke Nukem Advance | -2.78 % | -1,308 | -2.59 % | -2.14 % (-2.59 .. -1.34) |
| Ice Nine | -2.68 % | -804 | -1.96 % | -2.07 % (-2.21 .. -1.94) |
| Top Gear Rally | -1.96 % | +550 | +0.06 % | 0.00 % (-0.24 .. +0.10) |
| Hot Wheels | -3.17 % | -708 | -2.03 % | -2.32 % (-2.73 .. -2.03) |
| Emerald battle (control) | -0.98 % | -748 | -2.94 % | -2.98 % (-7.42 .. -0.45) |
| FireRed (control) | -0.90 % | -1 | -0.38 % | -0.22 % (-0.38 .. -0.02) |

Top Gear Rally is flat: its instructions fall like the others' but its translated-code misses rise (it diverges
by timing at frame 212, before the window, so this is two different executions). Recovery is cheap: 3-18 C
lookups a frame (the front cache answers the rest).
