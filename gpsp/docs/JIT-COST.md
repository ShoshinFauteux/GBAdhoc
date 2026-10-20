# JIT_COST: the cost of producing translated code

Branch `claude/jit-cost` (from `claude/bench-xlat` 56c55e2, XLAT_FAST). Root-make switch
`JIT_COST=1`, OFF by default. Pieces can be compiled out with `JIT_COST_NO=PUBINL:SCANWIN:SCANCOND`.
Each piece also has a run-time switch, so one binary can A/B it with the code layout held fixed
(harness keys, frontend built with `FE_EXTRA=-DJIT_COST_KEYS`):

| key | values | piece |
|---|---|---|
| `jit_pub_inline` | 1 (default) / 0 | PUBINL: publish with user-mode line ops / the kernel calls |
| `xlat_scan_fast` | 1 (default) / 0 | SCANWIN + SCANCOND RAM ARM scan / the original scan |

None of the pieces changes what is emitted, which tags are written or when anything happens.
Only the host work spent doing it changes.

## Where the time goes (twin, Doom II, ship + XLAT_FAST)

- About 285 publishes a frame. Each is one span of about 18 lines (1.2 KB) and already covers a whole
  eager translation burst: block exits are resolved recursively before the lookup returns, and
  the whole burst lands in one contiguous span. Each publish comes right before a jump into the code
  it publishes (`block_lookup_address_*` publishes, then returns the pointer). So **the publish
  count cannot be batched further without changing when code runs**. What can be cut is the
  cost of each publish.
- xlat zone: `translate_block_arm` 68%, `xlat_scan_arm_ram` 27%. Inside `translate_block_arm`,
  about 24% is twin-only bookkeeping (`drprof_note` / `drprof_class_arm`, DRPROF_TWIN). Twin xlat
  deltas therefore UNDERSTATE the PSP effect. Of the rest, the per-instruction loop overhead
  (cycle add, update_cycles test, cache-limit test, cheat-hook test, condition tests) is about 23%,
  spread over eight statements of 2-7 instructions each. They are mostly spilled locals. The emitter
  bodies are spread thinly over hundreds of statements. No single hot spot is left.
  (Method: a twin built from a preprocessed, statement-per-line cpu_threaded.c, so addr2line resolves
  inside the emitter macros.)
- The scan was about 55 host instructions per guest instruction: about 12 for the gate-bitmap test and
  12-15 for the flag-write switch.

## PUBINL

The old path was `sceKernelDcacheWritebackRange` + `sceKernelIcacheInvalidateRange`: two syscalls per
publish. Below 16 KiB their bodies are exactly a `cache 0x1A` loop + sync and a `cache 0x08` loop
(uOFW sysmem). Those two ops have always run in user mode on every PSP: `emit_phand` patches a
`jal` and publishes it that way. PUBINL issues the line ops itself:
- the lines are every line of every recorded span, rounded out as before;
- the order is all D write-backs, then a sync, then all I invalidates;
- whole-I cases (ownership change, or a span >= 16 KiB) still use `sceKernelIcacheInvalidateAll`.
  A hit loop has no large-range path, so the firmware's >= 16 KiB no-op cannot be reached;
- publish points are unchanged, so no code can run unpublished that did not before.

Kernel cache calls on Doom: about 570/frame -> about 11/frame (only the whole-I invalidates are left).
The twin cannot time cache ops; the hardware probe below does.

## SCANWIN / SCANCOND (second RAM ARM scan, `xlat_scan_arm_ram_fast`)

The body is word for word that of `xlat_scan_arm_ram`. Only the scan's hook macros
(`SCAN_VARS`, `SCAN_SET_CONDITION`, `SCAN_GATE_END`) are redefined around its expansion.
- **SCANWIN**: the SMC/entry gate bitmaps are tested only while `block_end_pc` is in a 64-byte window
  whose 4 bitmap bytes are not all zero. A zero byte fails both original tests for every pc it
  covers, and the maps do not change during a scan.
- **SCANCOND**: `arm_set_condition`'s 0x20 depends only on opcode bits 20-27 and 4-7. That is one bit of a
  256x16 table, built at first use **by running the macro itself** on every combination. Twin
  builds also check the table against the macro on 200k random opcodes.

Scan: about 55 -> about 44 host instructions per guest instruction (-12% scan on Doom).

## Evidence

- Twin (ship + XLAT_FAST): hash IDENTICAL on all 20 twin fixtures. xlat instructions: Doom -3.2%,
  Duke -3.4, Smashing -3.2, SSX3 -4.1, NFSMW -2.6, NFSC -2.3, Ford -3.5, HotWheels -3.2, MaxPayne -3.8,
  MTennis -2.1, TopGear -2.7. Light titles change by <0.5% of core.
- Coherency checker (tools/jitcoh, every model incl. 4 layout offsets): PUBINL gives 0 incidents on
  doom2/duke/hns_heavy/aw2, and on doom2/duke/smash/hns_heavy merged with claude/bench-dr
  SMC_PARTIAL_SINGLE (STABLE_THUNK retirement publishes). The mutants are CAUGHT:
  `brk-pubinl-ifirst` (I-invalidate skips a span's first line) 51k incidents on doom2;
  `brk-pubinl-dfirst` (D write-back skips it) 85k.
- Corpus oracle (35 ROMs x 3000 frames, power-on): `jcost` 35/35 PASS. Mutants `m_jc_win` (SCANWIN
  drops a window byte) caught 2/35, `m_jc_cond` (SCANCOND drops opcode bit 7) caught 5/35. The corpus
  cannot see PUBINL (qemu is coherent), so the checker mutants above cover it.
- PPSSPP vhash (harness, 1200 frames or the fixture's script): 26/26 GBA bench-ready fixtures IDENTICAL
  to 4.1.1 (runs/validate-20261019-213754), and all 36 incl. GB identical to XLAT_FAST x7.
- Switches off: release EBOOT byte-identical. Link harness .text/.data identical; only the
  `__TIME__` string differs, as it does between any two harness builds.

## Hardware (not run by the agent)

`icache_inval_probe = 1` on a JIT_COST_KEYS build adds these modes:
- `u_dnone` (control: no write-back, must show stale on a console);
- `u_d`, `u_i` and `u_pub` (user-mode write-back / invalidate / the core's `jit_pub_lines`; must be 0);
- `pubcost` lines: ns per publish of 4/18/64 dirty lines by kernel vs user.

Then interleaved ABBA perf runs of one PRX with `jit_pub_inline` / `xlat_scan_fast` 0 vs 1.
