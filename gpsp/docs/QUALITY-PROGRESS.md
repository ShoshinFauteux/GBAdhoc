# Quality roadmap — what is actually done

Audited 2026-09-18 against `QUALITY-ROADMAP.md`, on
`opus/performance-stability-fixes`. Scope was the items that do not need the
maintainer's participation, with the emphasis on §1.

**Headline: §1 is about half done. Four of its seven groups are complete; the
decomposition group — the one the 9/10 criterion is written about — is barely
started, and `main_psp.c` is still 6,413 lines.**

---

## Complete

### Simplify build profiles — 7/7

`release` / `harness` / `diagnostic` are named once, in `tools/build.sh`, instead
of in four drifting texts. `gpsp_profile.h` makes twelve silently-wrong flag
combinations compile errors (all five sampled were verified to fail; the real
release combination passes). A pixel-format mismatch between core and frontend is
a link error, and the ME PRX — a third, previously hardcoded copy of that
decision — now inherits it. Every build writes `psp/build-manifest.json` with the
profile, expanded flags, source commit and tree, toolchain digest, clean-tree
flag and artifact hashes. Release audits require the marker proving ADR-0067's
automation neutralisation is compiled in, rather than trusting it.

### Eliminate warning debt — 6/6, criterion met

68 → 0 project-owned warnings, and `tools/build.sh` now fails a build that adds
one (`tools/warning-allow` holds documented exceptions; it is empty). Six were
real defects, including `cheats.c` scanning `%08x` into a `u32` — same width on
MIPS32, four bytes into an eight-byte object on a 64-bit host. The rest were
telemetry inputs, marked `FE_EVT_ONLY` rather than cast to `(void)` so they read
as "input to a disabled reporter" and not "delete me". Proof it changed nothing:
the release ELF was byte-identical across the cleanup.

### Document subsystem contracts — 7/7

`docs/SUBSYSTEMS.md`, 258 lines: a diagram of what runs where (main CPU, Media
Engine, GE, audio thread, ad-hoc threads, io thread, storage), a table of every
field crossing a thread or processor boundary **with its handoff mechanism**, and
the contracts for display buffers, ME capture, dirty pages, SMC invariants, RFU
ownership and config migration. It also records where the seams are *not* clean,
so the next person does not rediscover it.

The ME contract is the one that paid for itself: I/O registers are captured per
scanline while OAM, VRAM and palette are one snapshot per frame, which is the
mechanism behind the one-frame sprite displacement.

### One command for host tests — done

`tools/run_host_tests.py`: 8 suites, PASS/FAIL/SKIP with a reason for every skip,
JSON manifest. Three were effectively unrunnable before — `rom_load` failed in
*every* worktree, and two had no runner plus a documented build command missing
`-ffunction-sections`.

### Inventory of dormant switches — done

`docs/BUILD-SWITCHES.md`: all 27 Makefile switches and 6 frontend defines with a
verdict. It found nine behaviour-changing switches with **no recorded verdict
anywhere**, and that `SMC_GATES_RANKED`'s Makefile comment described a promotion
threshold the code does not have — anyone tuning that 256 would have been tuning
nothing.

### Developer documentation — 5/7

`SUBSYSTEMS.md`, `BUILD-PROFILES.md`, `BUILD-SWITCHES.md`, `DEBUGGING.md`
(symptom → first thing to look at, ordered by how often each has actually
happened), `BUG-REPORT.md`. `ARCHITECTURE.md` keeps its netdrv content and gained
a pointer table so the documentation has one entry point.

### Tag and archive the accepted candidate — done

`candidate/stability-7283f13`, with the archived binaries in
`builds/unbound-stability-release-candidate-7283f13/` and an on-device rollback
(`EBOOT-7283f13-accepted.PBP`) on all three consoles.

---

## Barely started

### Turn `main_psp.c` into a coordinator — 2/8

| item | state |
|---|---|
| fast-forward state and scheduling → `ff_psp.c/.h` | **done and CLEARED**, 1,052 lines; hardware A/B measured +0.1% to +0.2% on PSP-1000 and PSP-3000 (n=15), i.e. neutral within noise, and it is **not** the cause of the +4% to +17% regression |
| ME capture/retirement/presentation/fallback → one renderer module | **interface closed, not moved** — lifecycle no longer touches `g_mer_*` |
| frame pacing and vblank policy → a pacing module | partial: pacing moved, `g_vc_target` deliberately stayed with the present loop |
| wireless session lifecycle → a networking coordinator | not started |
| Mystery Gift transport separate from RFU trading | already true; now documented |
| suspend/resume/relaunch → a lifecycle module | **unblocked**: renderer coupling cut from 10 symbols (8 variables) to 6 functions; relocation measured and still declined |
| harness-only execution → its own compilation unit | **measured, declined** |
| `main_psp.c` = init plus the top-level loop | **not achieved: 7,462 → 6,413 lines** |

Two of those are deliberate refusals with measurements behind them, not oversights:

* **The ME renderer/lifecycle interface is now closed; the relocation is still
  declined.** The interface change came first, as designed. The lifecycle half
  (`power_cb`, `wake_snapshot`, `standby_note`, `me_standby_step/down/up`) is 215
  lines across six ranges. It used to read and write renderer state directly --
  10 renderer symbols inbound, 8 of them raw `g_mer_*` variables, 24 references;
  it is now 6 symbols, all functions, no variables, 11 references. Total symbols
  crossing the boundary: 23 → 19.

  Moving those 215 lines into `lifecycle_psp.c` would still cost ~12 crossing
  symbols, against the fast-forward move's 1,052 lines for 6 inbound. So the
  encapsulation shipped and the relocation did not. The prior figure quoted here
  ("38 symbols, 22 variables") was measured without stripping comments and over a
  range that swallowed the renderer's own global declarations; it overstated the
  boundary.
* **The renderer's remaining coupling is outbound, and one third of it is on the
  frame path.** 489 lines, 5 symbols inbound, 19 outbound (10 of them variables
  read directly from elsewhere). The ini binding and the status reads are a
  clean accessor conversion; the 12 frame-path stage-ownership sites are not,
  and moving them without a hardware A/B would repeat the regression we spent
  this session explaining. Measured and recorded in `SUBSYSTEMS.md`.
* **One duplication removed from the stage-ownership protocol.** The same-frame
  retirement rule was written out twice in the main loop -- byte-identical
  apart from indentation -- and is now `me_rend_retire_if_idle()` in the
  renderer. Two identical copies of an ownership protocol is one edit away from
  two different protocols. The path is harness-only (`me_sameframe` is a harness
  ini key, and ADR-0067 makes every harness key return its default in a playable
  build), so it is unreachable in shipping builds and carries no player-facing
  risk.
* **The obvious harness slice is a bad trade.** `run_testpat` and `run_nettest`
  are ~150 lines but need 8 symbols from `main_psp.c`, including
  `plat_video_frame` and the Mystery Gift status strings. Compare the
  fast-forward move: 1,052 lines out for 6 inbound symbols.

### main_psp.c cold decomposition: CHECKPOINT REACHED, 9f58f43

`main_psp.c` is close to exhausted of cold boundaries worth taking, and that is
a finding rather than a failure to continue.  Two boundaries paid for
themselves -- `ff_psp.c` (1,052 lines, 6 crossings) and `paths_psp.c` (83 lines,
single-writer ownership of the on-disk layout).  What remains is either on the
frame path or is boot *policy* written inline in `main()`.

**Three boundaries measured and DEFERRED.**  Counts are from a full call-site and
guard survey of the 6,414-line file; the loop is lines 5492-6352.

| candidate | lines | owned state | shared state | in-loop | verdict |
| --- | --- | --- | --- | --- | --- |
| harness/diagnostic instrumentation | 406 | 21 vars | 2 | **7 of 11 functions** | deferred: Tier D for an organisational gain |
| Mystery Gift orchestration | 218 | 5 vars | **7** | `mgift_frame` every iteration | deferred: ownership ambiguous, feature works |
| harness short-circuit modes (`run_testpat`, `run_nettest`) | 119 | 0 | 0 | none, by construction | deferred: 6 crossings for 119 lines is too little ownership gain |

Notes that matter for anyone revisiting these:

* **No symbol in any of the three groups is behind a preprocessor guard.**  All
  compile into release, harness and diagnostic alike, so there is no profile
  isolation to exploit and no way to touch the first two without entering the
  frame path.
* Mystery Gift is the *worse* Tier D candidate despite being half the size: four
  of its seven shared variables (`g_rfu_last_cmd`, `g_rfu_last_state`,
  `g_rfu_seen_bcrd`, `g_rfu_seen_conn`) are written from the RFU trace callback
  and read by `mgift_frame`.  That is genuinely ambiguous ownership, not merely
  wide coupling.
* **Correcting an earlier figure recorded here:** the harness slice was described
  as needing 8 symbols including the Mystery Gift status strings
  `g_mg_l1/l2/linked/secs`.  Measured, `run_testpat` + `run_nettest` need **6**
  (`plat_video_frame`, `dump_ge_bmp`, `adhoc_stats_evt`, `log_adhoc_up`,
  `net_error_evt`, `g_drew`) and touch no `g_mg_*` at all.  The conclusion is
  unchanged; the number was wrong.
* `%s/log/standby.log` is live **only in the harness profile**: `standby_note()`
  compiles to `(void)fmt;` when `GPSP_NO_TELEMETRY && !GPSP_STANDBY_NOTES`, and
  `GPSP_NO_TELEMETRY` is derived from `GPSP_PLAYABLE` unless
  `GPSP_KEEP_TELEMETRY` is set.  `GPSP_STANDBY_NOTES` is set by no profile -- it
  is an escape hatch to force the notes back on, not dead code.

**Structural work is no longer justified by line count.**  It resumes only in
service of a specific correctness, stability or diagnostic objective.

### Reduce global-state coupling — 3.5/7

Done: the FF context (18 of 23 state variables now private to `ff_psp.c`, the
other five reachable only through accessors), thread/processor ownership
documented, every ME-shared field identified. Not done: renderer context,
wireless session context. Partial: `ff_psp.h` still declares 6 host externs
rather than a query interface — deliberately, so the move stayed a pure
relocation, but it is unfinished work and the header says so.

### Remove historical experiments — 2.5/6

Inventoried and marked, but **nothing was removed**. The nine no-verdict switches
are still there; the long historical narratives are still in `cpu_threaded.c`
(and I added more). Only "clearly mark generated files that must remain" is fully
done (`psp/me/README.md`).

### Consolidate entry points — 4/6

Missing: one command for the PPSSPP correctness tests (still several scripts
under `tools/e2e/`), and a single hardware acceptance checklist.

---

## Where I did not follow the roadmap's own rules

Worth recording, because both contributed to shipping a regression.

**"Require identical frame/audio results before and after structural changes" —
not done.** I verified the fast-forward move was *textually* verbatim and that it
compiled and passed the host suites. I did not verify frame or audio identity,
and I did not measure it on hardware before shipping. Hardware later showed
+4–13% work per frame.

**"Do not change module boundaries during active performance experiments" —
violated in spirit.** The `ff_psp.c` extraction and the dynarec freeze fix went
onto the same branch and shipped in the same build, so when performance dropped
there were two candidate causes instead of one. Round 2 of the A/B exists purely
to undo that entanglement.

**"Add characterization tests before moving each subsystem" — partial.** I added
`tools/test_smc_safety.c` for the dynarec invariants (461 checks, asserting the
pre-fix behaviour too). The pacing engine got no characterization test before it
moved, which is exactly the subsystem that regressed.

---

## Not attempted (needs the maintainer, ROMs, or hardware time)

§2 compatibility suite, §3 soaks and failure injection, §4 trading and Mystery
Gift matrices, §5 the hardware benchmark expansion, §7 UI work, §9 the candidate
checklist. §8's README cleanup is the maintainer's in-flight work in the outer
repository and was left alone.

Two small items that do not need hardware and were simply not reached: marking
superseded ADRs, and a dedicated release/rollback procedure document.

---

## What I would do next, in order

`ff_psp.c` is **cleared**, so the roadmap's "do not start the next extraction
while a regression is unexplained" rule no longer blocks decomposition. The
process lesson it taught stands and is recorded below: it was combined with the
dynarec work in one step, so neither could be measured alone.

1. **The `unbound_double_high` residual is still open** — the shared-stub arm
   keeps a reproducible +3.5% to +3.65% on both consoles. It is independent of
   `ff_psp.c`. The clean experiment is 7283f13 plus **only** the
   `BADJUMP_REPORT` gating change; static code-size inspection (the dispatcher
   shrinks 720 → 568 bytes and `badjump_recover` stops being a symbol) makes
   code layout a plausible common variable but **does not establish causality**.
   Do not attribute the residual to layout without that source-pinned A/B.
2. **Reconcile documentation before the next extraction.** Several claims in
   these docs described withdrawn experiments as current behaviour; a detailed
   stale comment is worse than none, because it steers the next refactor.
3. **Continue with cold, non-frame-path code first** — configuration and
   orchestration — then isolated feature coordinators. Renderer, lifecycle and
   dynarec last, each gated on the hardware evidence its risk demands.
4. **The output oracle covers less than its name suggests.** See the coverage
   table in `SUBSYSTEMS.md`: it exercises the CPU renderer only, and cannot
   reach ME execution, suspend/resume or same-frame retirement.
