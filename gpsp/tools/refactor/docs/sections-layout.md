# Sections and layout tools: T2, T5, T10

These are phase T tools for the main_psp.c restructure (`gpsp/docs/design/RESTRUCTURE.md` §0.1, §5, phase T). They are written in host Python, use no docker and need no build. They read ELFs through `elf32.py` and GNU ld maps through `sl_ldmap.py`.

| Tool | Gate | Proves | Inputs |
|---|---|---|---|
| `sect_cmp.py` (T2) | G1 | Two builds are identical in every loadable byte | two ELFs or flavour dirs |
| `hotord_check.py` (T5) | none (layout hygiene) | The pinned order is still true of this build | a flavour dir, or an ELF with an optional map |
| `layout_report.py` (T10) | G3 | Where the layout-sensitive things are, what would re-pin them, and that no function reached an unplanned input section | a flavour dir, plus an optional REF |

Helpers:
- `sl_ldmap.py` is the map reader. It records output sections, input sections with the linker-script line that placed each one, and map symbols.
- `sl_dwline.py` is the DWARF line-row decoder, vendored from `scratch/x4_dwline.py`.

The `sl_` prefix keeps these clear of the other groups' modules.

All tests are in `tests/sections-layout_*`. `bash tests/sections-layout_run_all.sh` runs every one of them on the clean baseline (`mainpsp-tools-out/baseline` beside the checkout, a local output not in the repo; 01067e74, three flavours built twice; see [baseline.md](baseline.md)).

A **flavour dir** is what `flavours.sh` writes for one flavour: `gpsp_adhoc.elf`, plus `gpsp_adhoc.map` when the sidecar ran, and `build-manifest.json`. Always compare release with release, and Lab with Lab.

---

## T2 `sect_cmp.py`: gate G1

```
sect_cmp.py A B [--expect-sections N] [--json OUT]    # ELFs or flavour dirs
sect_cmp.py --runs RUN1 RUN2 [--expect-sections N]    # every flavour in both
sect_cmp.py --list ELF|DIR [--expect-sections N]
sect_cmp.py --selftest ELF|DIR
```

Exit codes:

| Code | Meaning |
|---|---|
| 0 | identical |
| 1 | a loadable difference |
| 3 | `could not fixup imports` found in a build log |
| 4 | the section count is not N |
| 2 | usage error |

**What it compares.**
- Every SHF_ALLOC section, by name and by occurrence. It compares type, flags, address, size, alignment, entsize and every byte. For NOBITS sections it compares everything except the bytes.
- The address order of the SHF_ALLOC sections.
- `e_entry` and every program header field.
- The file bytes of every PT_LOAD segment. This catches a loaded byte that lies in no section.

Every differing range is named by its symbol. When the dir has a map, it also names the input section and object, for example `update_gba+0x108 [.text.update_gba gpsp_libretro_psp1.a(main.o)]`.

For a flavour dir, it also searches `build.log` and every file in `logs/`, dotfiles included, for `could not fixup imports`, the psp-stub-order trap. The make output that holds the warning is `logs/.build-eboot.log`; the first version globbed `logs/*`, which skips dotfiles, and missed it (fixed in verification, with a selftest mutant).

If the loaded bytes of a PT_LOAD segment differ while every section compares equal (a section header pointing at other bytes than the segment loads), that is a difference too. `--runs` fails a flavour that has an ELF in only one run dir; the first version skipped it silently.

**What it does NOT prove.**
- It says nothing about non-alloc sections (DWARF, `.comment`, symtab). These are listed as "informational" only.
- It does not compare the EBOOT or the PRXs. `det_check.py` reports them.
- It shows identity, not equivalence. Any change that moves code fails it by design, even when the change is harmless. T1 `func_diff.py` is the tool for "identical modulo address".
- The log check sees only the logs it is given. A bare ELF has no log.

**Mutant coverage.** The selftest runs on all three baseline ELFs and makes 86 checks per ELF. All of them pass.
- It flips a byte at the first, middle and last position of all 22 sections that hold bytes, including `.sceStub.text`, `.rodata.sceModuleInfo`, `.lib.stub*`, `.lib.ent*`, `.init`, `.fini`, `.ctors`, `.dtors`, `.eh_frame`, `.MIPS.abiflags` and `.data.pin`.
- It changes the size, address and alignment of each NOBITS section.
- It moves `.rodata.sceModuleInfo` with the same bytes.
- It clears SHF_ALLOC on `.sceStub.text`.
- It renames `.lib.stub`.
- It changes `e_entry` and `phdr[0].memsz`.
- It flips a loaded fill byte.
- It plants `could not fixup imports` in a log.
- Controls: byte flips in `.comment`, `.debug_info`, `.debug_line`, `.symtab` and `.strtab` must pass, and a clean log must pass.

Each mutant is cross-checked against stage 1's `det_check.py`. That tool must never catch a difference that `sect_cmp.py` misses, and it never does. `det_check.py` does miss three things T2 catches:
- NOBITS alignment changes.
- The loaded fill byte at file offset 0x13c. This is the 4-byte gap between `.init`, which ends at 0x13c, and `.text`, which starts at 0x140. It is loaded, but it belongs to no section.

### Acceptance (PLAN §5 T2)

| PLAN says | Result on the clean baseline |
|---|---|
| identical on two clean rebuilds of 01067e74 | **Reproduced.** `--runs run1 run2`: release, Lab and harness are all IDENTICAL. Each has 24 SHF_ALLOC sections (2 NOBITS) and 3 PT_LOAD segments. |
| 25 sections listed | **Does not reproduce: there are 24.** `--list` finds the same 24 in all three flavours and in the old `pin144-rel`: `.MIPS.abiflags .init .text .fini .sceStub.text .lib.ent.top .lib.ent .lib.ent.btm .lib.stub.top .lib.stub .lib.stub.btm .rodata.sceModuleInfo .rodata.sceResident .rodata.sceNid .rodata .eh_frame .ctors .dtors .data.pin .data .data.gbahot .sdata .sbss .bss`. `.reginfo` is not SHF_ALLOC. The run-all script asserts `--expect-sections 24` and checks that 25 is rejected with exit 4. PLAN §0.1 G1 and §5 T2 should say 24. |
| seeded changes in `.sceStub.text` and `.rodata.sceModuleInfo` caught | **Reproduced.** Three positions in each, plus the address move of `.rodata.sceModuleInfo` and the SHF_ALLOC drop of `.sceStub.text`. |

**Finding: `pin144-rel` is the same ELF as a clean pad-144 build.** The elf-func group built 01067e74 from clean with `LAYOUT_PAD_BYTES=144` (`workingTreeClean: true`, copied to `scratch-sections-layout/pad144-release`). It is byte-identical to the beta.4 kit's `pin144-rel/gpsp_adhoc.elf` (a local kit, not in the repo): both have sha256 `db6d625c…`.
- So the dirty tree flagged in PLAN §0.2 did not change that ELF.
- The 21 sections that differ between `pin144-rel` and the clean release come from the pad alone. That includes the 4,255 differing `.data` bytes, which are pointers into the moved `.rodata`.
- x4's figures measured on `pin144-rel` therefore stand for a clean pad-144 build.

---

## T5 `hotord_check.py`: is the pinned order still true?

```
hotord_check.py --build DIR [--order F] [--ord F] [--sizes fail|warn|off] [--no-map] [--json OUT]
hotord_check.py --elf ELF [--map MAP] ...
hotord_check.py --selftest DIR
tools/layout/regen.sh --check-only        # opt-in: check the committed order, exit 0/1
tools/layout/regen.sh --check [BUDGET]    # opt-in: report, regenerate, require the new order to pass
```

`--order` and `--ord` default to this tree's `psp/layout/hot_order.txt` and `hot.ord`. Check a build against the order it was linked with.

It checks every hot_order.txt entry:

- **match: the entry places exactly one input section.**
  - With a map, this is counted on the map itself. ld prints each hot.ord line and, under it, the sections that line placed.
  - Extra sections are classified. A live instance-A function is a failure. An instance-B partner copy (the `gbab_fe_*` objects that `EXCLUDE_FILE` does not exclude) or a dead weak default (no symbol at its address) is reported but does not fail.
  - Without a map, which is regen.sh's case, it counts the instance-A STT_FUNC symbols with that name. B's statics keep their names. They are told apart by the nearest marker globals on both sides being `gbab_*`. On all three baselines this agrees with the map on all 5,163 / 5,327 / 5,246 functions, and the selftest asserts it.
- **rename: an unmatched entry is checked for a new name.** If its base name, with `.isra/.part/.constprop/.cold` stripped, exists under another name, it is reported as `renamed: now X`. If the function now sits in `.text.unlikely.X`, `.text.startup.X` or similar, that is reported too.
- **B line:** if `gbalink_core_b.o` has a `.text.X`, then B's own line must place it.
- **size:** the listed size is compared with the ELF size. A difference in the 32-byte-rounded size is a **rounding break**, which breaks the "keep its 32 B-rounded size" rule in LAYOUT-PINNING.md.
- **ord:** `hot.ord` must equal what `gen_layout.write_ord` produces from `hot_order.txt`.
- **placement (added in verification):** instance A's uniquely named entries must sit in ascending address order (the longest ascending run is kept, the rest are `PLACE` failures), each within 16 KiB of the previous entry's end (baseline maximum 3,508 B). This is what catches, in symtab mode, a function re-sectioned to `.text.unlikely.X` that keeps its name: it is linked elsewhere. It only holds for an ELF linked with this order, so `regen.sh --check`'s post-check (a new order against the old ELF) passes `--no-placement`.

Exit 1 on any match, rename, placement, B-line or ord failure. With the default `--sizes fail`, any rounding break also exits 1. `--sizes warn` reports sizes without failing.

**What it does NOT prove.**
- It does not say whether the order is still the right one for speed. The profile weights are not re-measured.
- It does not show that a rounding break moved anything that matters. `pad_check.py` and T10 show what moved.
- In symtab mode (no map), it cannot see input-section names. A function re-sectioned to `.text.unlikely.X` but keeping its symbol is caught only by placement (its address), not named as re-sectioned. Use a flavour dir with a map, or T10's audit, for the section name.
- It is not wired into the link. The task allowed only an opt-in flag in regen.sh, and the default build is unchanged. A plain `regen.sh` behaves exactly as before; the test proves it never calls the checker. `regen.sh --check-only` is the one-line step a build, CI or `flavours.sh` (T15, another group's tool) can run to fail on a stale or renamed order. Wiring it in is the integrator's call.

### Acceptance (PLAN §5 T5)

| PLAN says | Result |
|---|---|
| reports 17 size mismatches / 13 rounding breaks today | **17 reproduces; 13 does not. The tool finds 12.** On the release baseline: 151 entries, 17 sizes differ, 12 rounding breaks. The 12 are `update_scanline`, `block_lookup_address_arm/thumb`, `block_lookup_translate_arm/thumb`, `rfu_update`, `smc_trap_entry`, `xlat_scan_arm_ram_fast.constprop.0`, `me_rend_retire_if_idle`, `rfu_frame_update`, `translate_block_thumb` and `translate_block_arm`. That is the same list x4's own prose names (its "refuted 1" list plus a.1's `me_rend_retire_if_idle`). Five entries differ in size but stay within their 32-byte rounding: `flush_translation_cache_ram_smc` (6536 to 6540), `audio_thread` (824 to 828), `smc_retire_span` (2792 to 2796), `fe_autopilot_frame` (2604 to 2612) and `memory_term` (348 to 340). No rounding rule (up, down, or the map's section size) gives 13, so "13" looks like a miscount in x4. Lab and harness: 29 differ, 24 break. The order was generated from a release build. |
| a deliberate `lat_swap` rename fails the build | **Caught; "the build" means regen.sh, not the link.** `lat_swap.isra.0` renamed `.isra.1` is caught three ways, each exit 1 with `RENAME … renamed: now lat_swap.isra.1`: in the ELF's string table (symtab mode), in the map as ld would print it (map mode), and through `regen.sh --check-only` against both the committed and a regenerated order. It does not fail the link, which was deliberately left unwired. |

**Also found today** (map mode, all three flavours, informational):
- 0 unmatched entries, 0 renamed, 0 ambiguous, and hot.ord consistent with hot_order.txt.
- 5 entries also pull extra bytes into the warm tail:
  - `fe_autopilot_take_mark`, `fe_autopilot_status` and `fe_autopilot_frame` place the partner's `fe_autopilot_b.o` copies (16 + 12 + 2,612 B).
  - `gpsp_rfu_now_us` and `gpsp_visible_done_hook` place dead weak defaults from `rfu.o` and `main.o` (32 B each).

**Other mutants, all caught:**
- `ll_present` re-sectioned to `.text.unlikely`.
- `frame_end` (two live statics) added to the order, caught in both map and symtab mode.
- B's `update_gba` not placed by its own line.
- `ll_sample` listed +32 B (a rounding break, fails) and +4 B (within its rounding: reported, passes).
- A hot.ord line deleted.
- A misspelt entry.
- An order refreshed from the ELF passes `--sizes fail`.

regen.sh test (`tests/sections-layout_test_regen.sh`, fake docker): C1, M1, C2, M2, M2b, C3 and M3 all pass. M3 is a regenerated order whose nm disagrees with the ELF, so the post-check fails.

---

## T10 `layout_report.py`: gate G3

```
layout_report.py CAND [--ref REF] [--plan FILE] [--objects REGEX] [--no-pad-check] [--json OUT]
layout_report.py CAND.elf --map CAND.map [--ref REF.elf --ref-map REF.map]
layout_report.py --selftest DIR [--ref OTHER_PAD_DIR]
```

**What it reports.** With `--ref`, every figure is also shown as a delta.
- The pads in use.
- `main`'s start mod 8192 and its I-cache set; also `layout_pad_cold`, `_end`, `rom_translation_cache_static & 8191` for A and B, and `gbahot_start`.
- Every data section's address mod 8192.
- The `.data.pin` size and headroom: how many bytes text and rodata can still grow before `.data` moves a page.
- The first 20 `.bss` symbols of main_psp.o, plus the watched `out`, `ring_r`, `ring_w` and `audio_ring`, with their D-cache sets.
- `main`'s loop set map by line: main_psp.c lines 16368 to 18090 inside `main`, x4's method.

**The input-section audit.**
- It lists the input-section name of every main_psp.o function from the link map. Use `--objects` to cover future fragments.
- It **fails on any `.text.unlikely*`, `.text.hot*`, `.text.startup*` or `.text.exit*` that the plan does not list**. The default plan is `.text.startup.main`. A `--plan` file holds globs such as `.text.startup.fe_*`.
- It also fails when there is no map, or when the audited objects have no sections. An empty audit never passes.
- It counts `.cold` symbols.

**Pad suggestions (with `--ref`).**
- Instance A's cold text is matched by name between REF and CAND: the `gpsp_libretro_psp1.a` functions linked after `layout_pad_cold`. So are A's tables, the `.rodata` objects. Only names unique in both ELFs are used.
- The commonest move mod 8192 is what the pad must take back: suggested = (CAND pad − move) mod 8192. The report also gives the share of anchors that the suggestion re-pins.
- Then it runs `tools/layout/pad_check.py` on the two ELFs. The nm files are generated from the symtab, and the `mips_stub.o` symbols come from the map. This shows hot-core and warm-tail moves, hot-core set changes, JIT mod 8 KiB, d(_etext) and d(_end).

**What it does NOT prove.**
- The loop set map here is **by line**. It does not cover the bytes of helpers inlined into the loop. T1b `loop_diff.py` owns the by-address window, and T16 owns whether a set change costs misses.
- The pad suggestions are arithmetic on anchor moves. They are not verified by a build: rebuild and re-run, then G1/G2 decide.
- The RO pad suggestion assumes the text pad is unchanged. A new text pad moves `.rodata` too, rounded to its alignment, and the report says so whenever the two interact.
- It gives no speed verdict. G5s and G5 decide that.
- The audit sees only the audited objects. The default is `main_psp.o`.

### Acceptance (PLAN §5 T10)

| PLAN says | Result |
|---|---|
| suggests pads that re-pin the 304/144 pair | **Reproduced.** REF is the clean release (304) and CAND is the clean pad-144 build: it suggests `LAYOUT_PAD_BYTES=304`, with A's cold text at −160 mod 8 KiB in 368 of 368 anchors. With the two swapped it suggests 144. pad_check: hot core 0/272 lines moved and the JIT at 0x1500 in both. A's tables sit −128 mod 8 KiB at pad 144, because `.rodata` is 64-byte aligned and absorbs 32 of the 160 B. The RO suggestion of 128 is flagged as valid only for an unchanged text pad; setting the text pad back to 304 restores `.rodata` as well. |
| flags a seeded `cold` function without a section | **Reproduced, with the real toolchain.** `tests/sections-layout_test_coldsec.sh` uses the build's own `pspdev/pspdev` image, `-O2 -G0 -ffunction-sections`, on a stand-in TU, not main_psp.c. psp-gcc emits `.text.unlikely.seeded`, and the default script links it at 0x08804018, **before** `.text.startup.main` at 0x08804038. That confirms the §0.2-5 hazard. layout_report then fails and names it. With `section(".text.fe_cold_ok")` it passes. An unplanned `.text.startup.fe_boot` fails until the plan lists `.text.startup.fe_*`. The same cases are also seeded into the real baseline map in the selftest, along with `.text.hot.*`, a bare `.text.unlikely`, another object's `.text.unlikely`, and the no-map and empty-audit cases. |

**G3 baseline figures** (01067e74, clean; identical in run1 and run2):

| | release | Lab | harness |
|---|---|---|---|
| `main` | 0x0893bb00, **mod 0x1b00**, set 108, 34,512 B | 0x0893d040, **mod 0x1040**, set 65, 48,448 B | 0x0893d040, **mod 0x1040**, set 65, 48,380 B |
| loop by line | **220 lines / 115 sets**, max 3 per set (x4 reproduced) | 273 / 123, max 4 | 282 / 123, max 4 |
| `out` (main_psp.o .bss) | `out.0` 0x08b96058 (matches PLAN's figure) | `out.1` 0x08bc005c | `out.1` 0x08bb605c |
| `.data.pin` | 28 B, headroom 27 B | 3,612 B, headroom 3,611 B | 2,412 B, headroom 2,411 B |
| JIT (`rom_translation_cache_static`) | mod 0x1500 (A and B) | 0x1500 | 0x1500 |
| `_end` | 0x0956198c | 0x095f198c | 0x095d998c |
| main_psp.o input sections | 161: `.text` 1, `.text.<fn>` 159, `.text.startup.main` 1 | 205 | 204 |
| unplanned special sections / `.cold` symbols | 0 / 0 | 0 / 0 | 0 / 0 |

The baseline Lab build uses the commit's own pads, 304 and 0. That differs from the Lab pads in LAYOUT-PINNING.md, so judge Lab only against a Lab REF built the same way.

---

## Running the tests

```
bash tests/sections-layout_run_all.sh                 # all of the below
python sect_cmp.py --selftest <flavour dir>           # 86 checks
python hotord_check.py --selftest <flavour dir>       # 16 checks; 6 of them need the map
python layout_report.py --selftest <dir> [--ref <pad144 dir>]
bash tests/sections-layout_test_regen.sh [<flavour dir>]   # regen.sh opt-in, fake docker
bash tests/sections-layout_test_coldsec.sh            # real psp-gcc in pspdev/pspdev (SL_LOCK=build.lock to serialise)
```

`run_all` result on 2026-11-05: **all 16 steps pass.** The steps are T2 list, determinism, the 25-is-wrong check, three selftests, and the 304-vs-144 difference; T5 three selftests, today's state failing, and regen wiring; T10 three selftests including the pad pair, and the real-toolchain test.

Inputs that were not built here:
- The pad-144 ELF is the elf-func group's clean scratch build, copied unchanged. It was built `--no-sidecars`, so it has no map, and the tools use the REF's map for classification.
- No PSP build was run for these tools.
