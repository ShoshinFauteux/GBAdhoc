# Compensating-pad probe (PLAN §0.2-9, phase T last bullet)

**Result: the probe was NOT accepted under PLAN's criterion.** LP passes: the loop's set map is restored
exactly. G1 fails: `.data`/`.bss` moved one 8 KiB page, so 22,592 instruction words *before* `main`
changed their data-address immediates. No code address before `main` moved. The cause is arithmetic,
not tooling, and the arithmetic also says which probe would pass (see "What this means").

## What was built

One scratch release build of 01067e74 (2026-11-05, under `build.lock`). Script, logs and measurements:
`scratch-padprobe/` (a local scratch dir, not kept in the repo; `probe_build.sh`, `probe_measure.sh`, `premain_diff.py`,
`release/`, `measure/*.txt`).

- **The only source difference** is one line added at the end of the ordered list in `psp/layout/hot.ord`,
  in the scratch export only (`release/hot.ord.diff`):
  `*main_psp.o(.text.layout_pad_cold)`. That places the existing never-called pad function right before
  `main`. No emulator source was edited. The rest of the tree is the `git archive` of 01067e74.
- `LAYOUT_PAD_BYTES=8188`, so `layout_pad_cold` is 0x2000 = 8,192 B (one I-cache way). The known shift
  is therefore **+8,192 B for `main`**, sized to hold `main mod 8192` constant.
- The rest of the environment matches the baseline: `build.sh release`, `RFU_SCHED=1`,
  `SOURCE_DATE_EPOCH=1793908328`, and the same build-id environment. The sidecar was clean (objects
  SAME 40 / SAME-NODEBUG 51 / DIFF 0, `relink-with-map SAME`), and there was no `could not fixup imports`.
  ELF sha256 `abc02d3e…706b`.

## What the gates show (reference: `baseline/run1/release`, sha `92e484e1…`)

| Gate / tool | Result |
|---|---|
| **LP** `loop_lpsim.py setmap` | `main` mod 8192: 6912 -> 6912. Loop window 376 -> 376 lines, **0 sets changed**, set histogram equal. **Restored.** As a control, the seeded +160 B shift (`--shift-b 160`) gives mod 7072, 377 lines and 5 sets changed |
| **G3** `layout_report.py --ref` | Loop by line: 220 lines / 115 sets, "set map IDENTICAL". `layout_pad_cold` moved -35,552 B (it now sits before `main`). `main` +8192. Hot core 0/272 lines moved; JIT `rom_translation_cache` still mod 0x1500. But `.rodata` moved +7,936, and `.data`, `.data.gbahot`, `.sdata`, `.sbss`, `.bss` and `_end` all moved **+8,192**: `.data.pin` had 27 B of headroom and the net `.text` growth was +7,904 B. A's cold text sits -288 mod 8 KiB (G3 suggests `LAYOUT_PAD_BYTES=284` to re-pin it). G3 itself passes: input-section audit clean, 0 `.cold` |
| **G1** `sect_cmp.py` | **FAIL**, 30 differences. Every section from `.fini` onward changed address. `.text` changed size by +7,904 B. `.init` differs in 3 bytes (crtbegin/crtend point at moved `.eh_frame`/`.ctors`). `.data.pin` grew 0x1c -> 0x11c. `premain_diff.py`: **22,592 of 319,152 words before `main` differ, in 204 functions** (e.g. `gbab_translate_block_arm` 6,621, `translate_block_arm` 5,160, `fe_host_run_frame` 285). Every one is a data-address immediate following `.data`/`.bss` +8192. So G1's "only `main` onward moved" does not hold |
| **G2** `func_diff.py --hot` | Address deltas: +0 for 253 functions (everything before `main`), +8192 for 4 (`main` and the 3 functions between it and the pad's old place), +7,884 for 3,020, +7,904 for 1,885, and -35,552 for 1 (the pad). **0 of 151 hot functions changed I-cache set.** 201 functions are DIFFERENT, all "lui never justified by a lo" or anchored-gap differences. Those come from the data move; func_diff is conservative there and correctly does not call them address-only |
| **G2L** `loop_diff.py` | Placement unchanged (window mod 900 -> 900, 0 lines changed set). LOOP CHANGED on 4 words. They are `lui t3,0x8b9` -> `0x8ba` (main+17384, +20328, +20344 inlined `blit_prof_evt`, +28572), base registers whose `%lo` users sit across a branch. The cause is again the data page |
| T5 `hotord_check.py` | 151 entries, 0 unmatched/renamed/misplaced. The 17 size / 12 rounding findings are the baseline's own |
| `det_check.py` | ELF and EBOOT differ, all three PRXs same, the 87 `.su` files same |

## What this means

The probe adds a net +7,884 B of `.text` (8,192 B before `main`, minus the 308 B pad that used to sit
after it). The 27 B `.data.pin` slack cannot absorb that, so the data segment moves a page: 8 KiB
more image, and 8 KiB less PSP-1000 boot heap (every boot-time reservation must fit the PSP-1000: gpsp/docs/MEMORY-BUDGET.md). One reordered pad
cannot hold both of these at once:

- `main` mod 8192, which needs the pad before `main` to be ≡ 0 (mod 8192) here, because nothing upstream changed, and
- everything after the pad's old place (A's cold text, `.rodata`, `.data`, `.bss`), which needs the pad's
  size to stay 308 B (≡ 308 mod 8192).

**In real use there is no conflict.** When hot or core-B text before `main` grows by Δ, shrinking a pad
that already sits before `main` by Δ restores `main` and everything after it, because the total size is
unchanged. The cost is the one-time move of the pad to before `main`. That move is either:

1. **Same size (308 B), only reordered.** Predicted, not built: `main` and the 3 functions up to the pad's
   old place move +308 B (`main` mod 6912 -> 7220, so the loop's set map moves once). Everything outside that
   range stays byte-identical, including `.data`/`.bss`. This is a G2L/LP/G5s event: G5s is rebaselined once.
   After that, the pad compensates as PLAN describes. **This is the variant that would satisfy G1's "only `main` onward moved".**
2. **Sized to keep `main` (this probe).** The loop set map is kept, but data moves a page (8 KiB heap) and
   G1 shows hot-code immediates changing.

Recommendation for P/HW-3 planning: variant 1, done as its own layout commit. Then re-tune the pad on every
pre-D1 size change before `main`, with G3 printing `main mod 8192` and LP checking the set map. A second
build is needed to confirm variant 1's prediction. Phase T was allowed one, so that build is still owed
and should be the first layout event of P.

Not shown by this probe: any timing. LP's simulation is still at placeholder rates (T16 open issue: the L
group is 100 % misses per frame), so "set map restored" is a static statement, not a measured miss delta.
