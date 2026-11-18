# ELF function tools: func_diff (T1), loop_diff (T1b), inline_census (T13)

These are phase T proof tools for the main_psp.c restructure (`gpsp/docs/design/RESTRUCTURE.md` §0.1 gates G2, G2L and IC, and §5).
All three are host Python 3 with no dependencies: no pyelftools, no docker and no psp-gcc. They read the linked `gpsp_adhoc.elf`.
Each runs in about 1 s on the baseline ELF.

| File | Role |
|---|---|
| `func_diff.py` | T1, G2: every function is the same bytes, the same modulo address, or different |
| `loop_diff.py` | T1b, G2L: the per-frame loop inside `main`, compared by address, with placement |
| `inline_census.py` | T13, IC: for each function, emitted, cloned, inlined into which callers, or dead |
| `ef_mips.py` | Shared address-normalising word comparison (from `scratch/x4_func_diff.py`) |
| `ef_dwarf.py` | DWARF 2-5 reader: `.debug_line` v2-5, and `.debug_info` v2-5 with abbrev, str, line_str, rnglists and ranges |
| `ef_srcscan.py` | Function-definition and `#if` scanner (from `scratch/x4_src.py`, with one fix, see T13) |
| `elf32.py` | ELF reader (T15 group, unchanged) |
| `tests/elf-func_test_*.py`, `tests/elf_func_lib.py`, `tests/elf-func_run_tests.sh` | Self-tests and mutant tests |

Run every test with `bash tests/elf-func_run_tests.sh` (about 40 s), or one tool at a time with `<tool> --selftest`.
The tests use the baseline in `mainpsp-tools-out/baseline/` beside the checkout (a local output, not in the repo; see [baseline.md](baseline.md)) and the pad-144 scratch ELF below. Override the paths with `EF_BASELINE`, `EF_PAD144` and `EF_SRC`.

**Scratch build for T1.** The pad pair needs a pad-144 release ELF of the same clean tree. The baseline holds only pad 304.
It was built once under the lock, release flavour only, without sidecars:

```
FLAV_RELEASE_ENV="LAYOUT_PAD_BYTES=144" flock $O/build.lock bash gpsp/tools/refactor/flavours.sh \
    --commit 01067e74 --flavours release --no-sidecars --out $HOME/mainpsp-tools-out/scratch-elf-func/pad144
```

The build took 109 s. Its ELF sha256 is `db6d625c…`, it was built from a clean tree, and it is copied to `mainpsp-tools-out/scratch-elf-func/pad144/release/` (local, not in the repo).

---

## T1 `func_diff.py` (G2)

```
func_diff.py BASE.elf CAND.elf [--file main_psp.c] [--names a,b] [--allow fn,..] [--allow-file F]
             [--hot psp/layout/hot_order.txt|hot.ord] [--hot-strict] [--data] [--strict-weak]
             [--any-flavour] [--json OUT] [-v]
exit 0 equivalent (or every difference allowed) | 1 difference | 2 usage/flavour mismatch | 3 --hot-strict and a hot fn changed set
```

It fits the T15 `flavours.sh --against` contract, `python3 func_diff.py REF.elf NEW.elf`, and it runs under WSL python3.

**What it proves.**
- Every function symbol is matched by (canonical name, STT_FILE for locals, occurrence). Each pair must be identical bytes, or identical once address operands are resolved to `symbol+offset`.
- The address operands it resolves are:
  - j/jal targets;
  - %hi/%lo pairs, including VFPU `lv/sv` (new);
  - pointer words in data (with `--data`);
  - anonymous C strings, by their text.
- Clone and static number suffixes are dropped from names (`.isra.0` becomes `.isra`). Opcodes, registers, PC-relative branches and every other immediate must match bit for bit.
- `--hot` lists every hot.ord/hot_order.txt function whose address mod 8 KiB (its I-cache set) or size changed. This is G2's "I-cache set of every hot.ord function".
- A release ELF is never compared with a lab ELF, because `flavour.json`/`build-manifest.json` sit next to each ELF.

**What it does NOT prove.**
- It does not cover timing or cache behaviour (G3, LP and G5 do), or data layout beyond sized `STT_OBJECT`s with `--data`.
- It does not compare anonymous `.rodata` between symbols except where code references it (G1/T2 covers those bytes).
- A **WEAK** pairing is a guess. It accepts a %lo whose %hi reached it through a move or a spill. Each weak site is listed: 35 on the pad pair, all with `-v`. `--strict-weak` turns them into failures for review.

**Changes from the prototype (`scratch/x4_func_diff.py`).**
1. **Prototype false negative, found by a new mutant and fixed.**
   - The mutant moves a real `%lo` in `audio_thread` from `audio_ring+0` to `audio_ring+4`, which is a different address.
   - The prototype accepted it: exit 0, "1 weak match". Its WEAK tier found a lui on *another* register whose address landed in zero-filled `.rodata`, and both sides tokenised to `ro:00000000 x8`.
   - Now a WEAK pairing may match only a token that names something. Anonymous-content (`ro:`) and unresolvable (`ABS?`) tokens are not evidence.
   - The x4 numbers are unchanged after the fix (below).
   - A stricter rule was tried and rejected: refusing WEAK whenever a same-register lui disagrees gave 11 false DIFFERENTs, because registers are reused.
2. VFPU memory ops (`0x32/0x35/0x36/0x3a/0x3d/0x3e`) are %lo users now. This was x4's known gap.
   - The clean ELFs contain **zero** VFPU memory ops, so the gap was moot here.
   - The decoder is tested with synthetic words.
3. Anonymous `.bss` addresses are tokenised as `section+offset-from-start` (x4 design item). Other changes: `--allow`/`--allow-file`, `--json`, `--hot-strict`, `--strict-weak`, the flavour refusal, and the weak-site list.

**Acceptance (PLAN §5 T1, phase T Verify)**, release 304 (`baseline/run1`) vs 144 (`scratch-elf-func/pad144`):

| | PLAN / x4 | Clean 01067e74 |
|---|---|---|
| Functions | 5,163 | **5,163** |
| Identical bytes | 140 | **140** |
| Identical modulo address | 5,022 | **5,022** |
| Differing | 1 (`layout_pad_cold`) | **1 (`layout_pad_cold` 308→148)** |
| Address deltas | −160 ×4,905, 0 ×258 | **same** |
| hot_order.txt functions that changed set | 0 / 151 | **0 / 151** |
| x4's 3 mutants | 3/3 | **3/3**, each caught both as A vs mut(A) and as pad304 vs mut(pad144) |

- With `--allow layout_pad_cold`, the exit is 0. The run takes 1.0-1.8 s.
- Extra mutants, all caught: `%lo +4` (above); a jal retargeted in a function that *moved* −160 B; a `.data` word flip (`--data` exit 1, while a code-only run ignores it); 3 synthetic VFPU cases; `--hot-strict` on a hot list containing a moved function (exit 3); release vs lab refused (exit 2).
- run1 vs run2 is all same-bytes in release, lab and harness (5,163 / 5,327 / 5,246 functions), and the data objects are identical.

## T1b `loop_diff.py` (G2L)

```
loop_diff.py ELF                    report
loop_diff.py BASE.elf CAND.elf      compare     [--loop-lines 16368-18090] [--func main] [--file main_psp.c]
                                                [--body-strict] [--json OUT] [-v] [--any-flavour]
exit 0 loop same | 1 loop window changed (or any byte of main with --body-strict) | 2 usage | 3 same code, placement moved
```

**What it computes.**
- **By line** (x4's method): rows of `main` on loop lines become 64 B lines, sets and the per-set maximum.
- **By address** (the new G2L window): it starts at the first and ends at the last address of the loop-line rows *and* of every `DW_TAG_inlined_subroutine` directly in `main` whose `DW_AT_call_line` is in the loop. Every byte of `main` in between counts, whatever line or helper DWARF gives it.
- Each window byte is attributed to one owner: loop line, loop-called inlined helper (by name), other main_psp.c row (boot/session code GCC interleaved), or other file.
- `main`'s start mod 8192, and the window's set map.
- The inlined-helper total, two ways:
  - by line rows before `main`'s definition line, which is x4:90's method;
  - as DWARF top-level inline ranges, as a cross-check.
- In a comparison, it runs:
  - pass 1: the window word by word (ef_mips normaliser), with *every* differing word attributed to `file:line` plus its inline chain (`helper@L:call`) and its owner class;
  - pass 2: main's whole body;
  - a placement check: main mod 8192, window offset and size, sets whose occupancy changed, and window lines that changed set.

**What it does NOT prove.**
- Anything outside `main` (that is T1).
- The per-frame miss cost. The window is an upper bound on fetched lines because it contains rarely run branches. LP/T16 models the cost.
- Anything about data.
- Pass 1 is by **offset within `main`**. If `main`'s size changes, the body pass reports "size", and the window is compared only when the window's size is unchanged. After an edit above the loop, an unchanged loop is reported as "window off X→Y, same-mod-address, placement moved" (exit 3), not as a code change.

**Acceptance (PLAN §5 T1b)**, clean release base (`main` @0x0893bb00, 34,512 B):

| | PLAN / x4 | Clean 01067e74 |
|---|---|---|
| By line (16368-18090) | 220 lines / 115 sets | **220 lines / 115 sets**, max 3 per set, offsets 10,372..34,432 |
| Inlined-helper bytes in main (before L:13837) | 9,912 | **9,912** (DWARF top-level instances: 9,992 B from main_psp.c functions, 10,752 B all) |
| `main` start mod 8192 | (G3 field) | **6,912 (0x1b00)** |
| **By-address window (new baseline)** | to be recorded | **off 10,372..34,432, 24,060 B, 376 lines over 128 sets (max 3), window start mod 8192 = 900** |
| Seeded edit in `audio_status_frame` | reported as a loop change | **exit 1**: `main_psp.c:857 inlined audio_status_frame@L:16714`, class loop-helper, caught as A vs mut(A) and as pad304 vs mut(pad144) |

- **Window composition:** 9,828 B loop lines, 7,032 B inlined loop helpers (60 instances), 6,980 B other main_psp.c rows inside the window, 220 B other files, 0 unattributed.
- The PLAN-named helpers are all inside it: `audio_status_frame` 168, `rfu_link_down_drain` 16, `exit_assist_frame` 268, `rfu_trace_drain` 108 and `turbo_frame` 400 B.
- The by-address window has **the same bounds** as the by-line window: on this base every loop-called inline range lies between the first and last loop row. The window is still bigger in *lines*: 376 against 220.
  - The by-line figure counts only lines holding loop-line rows.
  - The window also holds the 7,032 B of helpers and 6,980 B of non-loop rows, including main's cold boot tail (+0x7620..+0x82b4, PLAN §0.2-4). GCC placed that tail inside the loop's span.
- **Consequence for PLAN §0.2-4, "edits limited to those [cold boot] rows leave the loop's offset alone":** this is true for the offset. But by G2L's own definition such an edit changes bytes inside the window.
  - loop_diff reports it as exit 1, with class `other-main-line`, so the reviewer can see it is not loop code.
  - A seeded edit on L:16093 at +28,360 shows this. 422 such rows lie in the window.
- The seeded `audio_status_frame` word carries line **857**, outside 16368-18090. A line-range diff would not have attributed it to the loop, which is critique R4, demonstrated.
- Other tests:
  - An edit on boot row L:14545 (+3,816, before the window) gives exit 0, with the body pass reporting CHANGED. With `--body-strict` it gives exit 1.
  - A synthetic move of `main` by +64 B with the same code gives exit 3: 376 window lines changed set.
  - run1 vs run2 is same-bytes in all three flavours.
  - In the pad 304/144 pair `main` does not move (the pad follows it), so the window is same-mod-address, with exit 0.
- **Lab and harness reports (recorded, no PLAN figure).**
  - Lab: `main` @0x0893d040 (mod 4,160); by line 273 lines / 123 sets; window 28,224 B, 442 lines / 128 sets.
  - Harness: same `main` address; by line 282 / 123; window 27,392 B, 429 lines / 128 sets.
  - The loop-line range 16368-18090 is the release one; the lab and harness figures use the same range.

## T13 `inline_census.py` (IC)

```
inline_census.py ELF --src psp/main_psp.c [--flavour release|lab|harness] [--list] [--json OUT]
inline_census.py ELF --src ... --against BASE.elf [--src-base OLD.c] [--allow fn,..]
exit 0 | 1 a change not allowed | 2 usage / flavour mismatch
```

**What it does.** For every function *defined* in the source file, it reports:
- whether the definition is live in the flavour (source scan);
- its emitted symbols, with clone kinds (symtab, using x4_map.py's selection);
- every inline instance from DWARF: count, bytes and **callers**. A caller is named by the ELF symbol holding the code, for example `us_to_vblank` → `lat_swap.isra.0` ×1, `lat_wait_step` ×7. For nested instances it also gives the inline parent.

The status is one of `emitted`, `emitted+inlined`, `clone`, `inlined`, `dead` (no symbol and no instance) or `compiled-out`.

`--against` reports each function whose status, symbol set, caller set or instance count changed, plus symbols that appeared or vanished with no definition in the file (header helpers).

**What it does NOT prove.**
- That code is equal (T1 does that).
- Inlining in other translation units: only the source file's CU is read.
- The source scan does not read headers. A `#if` on a macro that only a header defines evaluates as undefined. `scan_disagreements` lists every function the scan calls not live although the ELF has code for it. It is 0 in all three flavours.
- The flavour define sets are x4_src.py's table, with `harness` = lab minus `LAB_HARNESS`. They are not read from the manifest.

**Scanner bug found and fixed (`ef_srcscan.py`, `JOIN_CONTINUATIONS`).**
- `x4_src.py` evaluated only the first physical line of a directive.
- `#if !defined(GPSP_PLAYABLE) || defined(GPSP_KEEP_TELEMETRY) || \` (main_psp.c:10198) then failed to parse and counted as false. So in lab/harness the 7 stall-watchdog functions counted as compiled out, although the ELF contains them.
- After the fix lab shows 328 live / 7 compiled out. Under the old behaviour the census reports exactly those 7 disagreements, and the test checks that.
- **Release counts do not change, so x4's release numbers stand.** x4's lab line counts (17,426 live lines and 2,329 lab-only) were computed with the old scanner and are understated by that block.

**Acceptance (PLAN §5 T13: "reproduces the counts")**, clean release:

| | x4 (b.2, x4_map) | Clean 01067e74 |
|---|---|---|
| Definitions / live / compiled out | 335 / 280 / 55 | **335 / 280 / 55** |
| Live with no symbol ("127 inlined") | 127 | **127**, and DWARF splits it: **121 inlined, 6 dead** (no instance anywhere) |
| Emitted symbols / clones / global | 159 / 22 / 25 | **159 / 22 / 25** |
| main_psp.c `.text` | 104,380 B | **104,380 B** |

- **PLAN's "127 inlined" is 121 inlined plus 6 dead-stripped.** x4 already hedged with "inlined or dead-stripped". The 6 are `sleep_sim_window_enter` (defined twice under `#if`), `me_rend_vmem_held`, `net_error_evt`, `adhoc_ctl_evt`, `rfu_tr_state_name` and `gblink_script_disconnect`.
- **Release status:** emitted 106, emitted+inlined 30, clone 17, inlined 121, dead 6, compiled out 54 (55 definitions, because one name has two definitions). There are 418 inline instances, 36,992 B.
- **Lab:** 328 live / 7 out; 132 with no symbol (all inlined); 203 symbols (25 clones, 26 global); 401 instances.
- **Harness:** 328 / 7; 133 inlined; 202 symbols (25 / 26); 400 instances.
- **Mutants, all caught (exit 1):**
  - `.strtab` rename `lat_swap.isra.0` → `.part.0`: both `lat_swap` and `us_to_vblank`, whose caller symbol changed, are reported.
  - `exit_cb` renamed away: reported as emitted → dead, plus an orphan symbol. `--allow` turns this run into exit 0.
  - In `.debug_info`, turbo_frame's only inline instance is re-pointed to `audio_status_frame`: turbo_frame inlined → dead, audio_status_frame 1 → 2 instances.
- run1 vs run2 in all three flavours, and pad 144 vs pad 304, give 0 changes.

## Notes for the integrator and the other groups

- `ef_dwarf.line_rows` now emits an end-of-sequence row, so the last row of each sequence gets its true end address. `scratch/x4_dwline.py` dropped it. The figures above are unchanged by this.
- `.debug_info` in this toolchain is **DWARF 5** for all 400 CUs. `.debug_line` is v3 for 396 CUs and v5 for 4. Tools that copy `x4_dwline.py` must handle v5 line headers. `ef_dwarf.py` does.
- T10/T16 can reuse `loop_diff.Loop`: window bounds, `wlines`, set map and composition. T12 can reuse `ef_dwarf` for call graphs.

## Verify pass (adversarial, 2026-11-05)

Scripts and logs (not kept in the repo): `scratch-elf-func-verify/` (`sweep_fd.py`, `sibling.py`,
`loop_mut.py`, `mkfrag.py`, `strmut.py`). Every acceptance figure above was re-run and reproduces
(T1 5,163/140/5,022/1 + 0/151 hot; T1b 220/115, window 10,372..34,432, 9,912 B; T13
335/280/55/127 (121+6)/159/22/25/104,380 B). Four false PASSes were found and fixed; the figures are
unchanged by the fixes.

| Hole (first version) | Mutant evidence | Fix |
|---|---|---|
| Tier-1 `%lo` pairing (lui on the base register itself) accepted any address in a zero-filled anonymous gap: `ro:` tokens compared content only | single-word sweep, pad 304 vs mut(pad 144), every 10th function: ~3.5 % of `imm±4/+1/+64` mutants passed (e.g. `addiu v0,v0,1` -> `+5` in `render_scanline_objs`); inside main's loop window **683 of 2,673** `%lo+4` mutants passed loop_diff | `ro:` tokens carry their anchor (`Space.anchor`: the sized symbol before the gap, or the section start) |
| Canonical names collide (`hint.14..17@ui_psp.c`, `pipe_fetch.constprop.0/.1`, `CSWTCH.*`; 1,774 names in release) | `sibling.py`: re-point a jal or a lui/lo pair to a same-canonical-name sibling: **17,577 of 17,577** passed | a shared canonical name gets `@STT_FILE` and, inside a file, `#address-ordinal` in tokens |
| WEAK pairing landing in two different 1-char strings (`")"`) | `lw +64` in `gbab_load_gamepak` passed | `weak_unsafe`: a string shorter than 4 chars must also match by anchor |
| `--data` compared sized objects only: an edited string literal / switch table / float constant behind byte-identical code was EQUIVALENT | `.rodata` "GBAdhoc version" byte flip, `--data`: exit 0 | `--data` also compares every anonymous gap of non-code PROGBITS sections, keyed by anchor, pointer words aligned by address (57 gaps release; 0 differ on 304/144) |
| T13 scanned main_psp.c only; PLAN §1.1 fragments (`fe/*.inc.c`, same CU) were invisible | moving 5 functions into fragments (ELF unchanged): 7 false changes, and `audio_status_frame` (inlined-only) had no row at all, so a change in its inlining between two split trees could not be seen | `ef_srcscan.expand_fragments`: quoted `#include` of an existing `.c`/`.inc` file is expanded in place (rows carry `src_file`/`src_line`); 0 changes after the move |

After the fixes: pad 304 vs mut(pad 144), every 10th function: 0 of 64,279 single-word
mutants pass; every 7th: 2 of 97,053, both re-point a `%lo` to another copy of the same C
string text (`"rate"`, a tail-merged suffix), behaviour-equivalent by the token design.
Same ELF (run1 vs mut(run2)), every 2nd function, with only the anchor fix: 3 of 479,401
passed, all sibling statics (`sh_wait_frame.constprop`, `shj_connect`), closed by the
canonical-name fix (each re-checked). Sibling 0 of 17,577; loop window 0 of 2,673 `%lo+4`
and 0 of 897 adjacent swaps. Adjacent-word swaps (a reordered step), lui+1 and j+4 mutants
were always caught, before and after. New tests: func_diff §8/§9, loop_diff §4e, inline_census §5.

Still open (not fixed, by design or cost):
- A lui changed and justified by one `%lo` user leaves its other, byte-identical `%lo` users
  unchecked. In a real layout shift that only arises when the shift is a multiple of 64 KiB.
- A WEAK pairing can still accept an immediate change that exactly equals the layout shift of a
  named symbol (35 weak sites on 304/144; `--strict-weak` lists them).
- `--data` keys anonymous gaps by their preceding symbol, so a refactor that reorders literals
  across symbols reports `anon_data` changes (conservative). It is off by default.
- T13's scan still does not read headers (scan_disagreements stays the guard); a `#include`
  of a fragment inside a `#if` is expanded and then judged by the same `#if` lines.
