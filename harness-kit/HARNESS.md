# The autonomous hardware harness — how it works and how to drive it

Two PSPs run a scripted Pokémon trade, hand their memory sticks back to the PC over USB,
receive a new build and config, relaunch, and repeat — **unattended, indefinitely**. Over
300 recorded runs went into the current settings. This is the only source of truth on this
project: PPSSPP and the desktop twin cannot reproduce PSP-1000 WLAN latency, memory-stick
stalls, or the two-console rate mismatch that turned out to be the central bug.

---

## 1. The control loop

```
   console                                     PC (hw_loop.py)
   ───────                                     ───────────────
   run the autopilot script
   exit (code + reason)
   flush SRAM, close log
   write handoff/RESULT.TXT   ──────────────►  sees RESULT.TXT appear
   expose ms0: over USB                        copies log + save + screenshots
                                               restores the GOLDEN save
                                               stages the next build/config
                                               writes handoff/CMD.TXT = RUN
                              ◄──────────────  Write-VolumeCache, then EJECT
   sees the eject, reads CMD.TXT
   relaunches itself  ─────────────────────►   (loop)
```

Console side is `psp/usb_handoff.c`; PC side is `hw_loop.py`. The relaunch uses
`sctrlKernelLoadExecVSHMs2` — plain `sceKernelLoadExec` **returns instead of replacing the
process** on CFW and will silently do nothing.

### Invocation

```bash
python hw_loop.py --host D: --join E: \
  --stage  <dir>            # copied onto BOTH cards before each run
  --golden <dir>            # host-/join- prefixed .SAV restored before each run
  --logs   <dir>            # where collected logs/saves/screenshots land
  --verify                  # decode the post-run save; require the party to differ
  --forever --keep-going \
  --timeout 86400
```

**Always pass `--verify`.** Without it a "pass" only proves the script reached its last
line. With it, a pass means a real trade occurred.

**Always pass `--timeout`.** `--forever` does *not* make the initial console wait unbounded;
`--timeout` defaults to 1800 s and the loop exits when it expires. This cost a full
overnight campaign.

Start it **detached** (`Start-Process` on Windows). Twice it died as a child of a shell
session that ended.

---

## 2. Staging: how you change what a run does

`--stage` is a directory mirrored onto both cards before each run. Filenames prefixed
`host-` / `join-` land on only that console with the prefix stripped.

```
stage-live/
  EBOOT.PBP                        → both cards
  emerald_tradecenter_host.inputs  → both (the console picks by its ini)
  emerald_tradecenter_join.inputs
  host-.gpsp-harness.ini           → D: as .gpsp-harness.ini
  join-.gpsp-harness.ini           → E: as .gpsp-harness.ini
```

`.gpsp-harness.ini` is **role-critical** and is refused if staged unprefixed — otherwise
both consoles become the same role and no session ever forms.

**Editing a file in `--stage` between runs is how you change arms.** No restart needed.

`loop.log` now records an md5 beside every staged filename
(`EBOOT.PBP@4479faa5`), so every run is self-identifying. Key a run's config off the
`staged:` line, **not** the `run N: waiting` line — staging happens *before* that line is
printed, so the announce line misdates the run.

---

## 3. The `.gpsp-harness.ini` keys that matter

| key | meaning |
|---|---|
| `script` | autopilot file to run |
| `host = 1` / `join = 1` | role. Exactly one per console. |
| `handoff = 1` | enable USB handoff. **Never set from CONFIG.INI** — a stale file must not toggle USB on a console someone is playing. |
| `handoff_window_s`, `handoff_park_s` | USB offer window; `park_s = 0` parks **indefinitely** |
| `handoff_max_runs` | run cap. **`0` does NOT mean unlimited** — it falls back to 20. Currently 100000. |
| `net_session_fps` | session rate clamp. **Currently 57.00.** See HANDOVER §2. |
| `net_session_fps_snap` | 0 = apply the request verbatim; 1 = snap to whole vblank divisors |
| `emu_prio` / `io_prio` | 43 / 44 (0x2B / 0x2C). The priority stack — biggest single win. |
| `rfu_poll_min_cycles` | 34952 = 1/8 frame. Throttles the client's radio polling. |
| `rfu_idle_poll_cycles` | 34952. Host mid-frame receive (ADR-0068). |
| `rfu_disc_defer` | 0. Deferred peer disconnect — **premise falsified**, ships off. |
| `rfu_rx_cap` | 0. **Falsified, do not re-enable.** See HANDOVER §4.2. |
| `core_phase` | 0 in scoring arms — the instrument costs ~500 µs/frame. |
| `preempt_prof` | 0 in scoring arms — 4 syscalls/frame. |

**A regression gate must test what ships.** Leaving an instrument on in a scoring arm has
produced at least one false conclusion here.

---

## 4. The autopilot

A line-based script driving the emulated pad and asserting on GBA RAM. Full grammar in
`docs/AUTOPILOT.md`. The ops that matter:

| op | behaviour |
|---|---|
| `press BTN n` | inject for n frames. **Open-loop — a swallowed press is invisible.** |
| `hold`, `mash`, `holdmash` | closed-loop: hold/mash until a RAM predicate matches |
| `waitram addr mask val timeout` | assert. Free when already true. |
| `logram name size addr` | record a value as `EVT ap_val` |
| `evt <name>` | drop a named marker |

### Hard-won rules

- **`press` and `holdmash` both inject 0 on their exit frame**, and `logram` samples the
  frame *after*. Bracketing a working leg therefore reads **0**. Use `hold` if you need the
  value at the moment of injection. This nearly produced a false kill of a true finding.
- **`holdmash` overshoots by exactly one tile.** It releases the frame its predicate
  matches, but a step is already committed. Reliable and deterministic — the current seat
  route *uses* the overshoot and corrects it with a counted press.
- **Use each primitive where it has evidence:** `holdmash` for multi-tile approach, counted
  `press` for a single tile, `waitram` as the assert.
- The Trade Center seat route took **seven revisions**. Read the comments in
  `fixtures/emerald_tradecenter_join.inputs` before touching it — each one records a failure
  mode and why the obvious fix was wrong.

### Emerald RAM addresses in use

```
gMain.callback2      0x030022C4     gMain.heldKeys   0x030022EC
SaveBlock1 ptr       0x03005D8C     (+0x00 x, +0x02 y, +0x04 map id)
gObjectEvents        0x02037360     stride 0x24
  slot 0 = receptionist, slot 1 = host player, slot 2 = join player
join x/y             0x020373A8 / 0x020373AA
host x/y             0x02037384 / 0x02037386
map ids              TradeCenter 0x1919, PC 2F 0x060A
```
Live coordinates are map coordinates **+7**. Trade Center spawns: join (13,15) → chair
(14,12); host (12,15) → chair (11,12).

---

## 5. The oracle

`--verify` decodes the post-run `.SAV` and requires the party to differ from the golden
baseline. **This is the only thing that proves a trade happened.**

`golden-saves/` holds `host-EMERALD.SAV` and `join-EMERALD.SAV` — **different parties on
purpose**, so a completed trade is detectable and visible. The loop restores them before
every run, which is what makes runs repeatable. **Do not delete this directory; the rig
depends on it.**

Note: `exit=0` is **not** a success flag. A run can complete the trade and then fail on the
walk-out, and a run can exit cleanly having traded nothing. Score by the oracle.

---

## 6. Reading the results

`summarize_log.py` turns a 40 KB log into ~30 lines. Its rule — *report extremes and
changes, never averages of averages* — is why every real finding here came from a maximum,
an asymmetry between the two consoles, or a counter that was zero and stopped being zero.

`ab_compare.py` buckets runs by config. Note it does not know about every newer override
key; check what it groups on before trusting a comparison.

**Seven ways these logs have been misread, each of which produced a wrong answer at least
once: see `logs/INDEX.md` §3.** The two that bite hardest:

1. The cards keep **independent run counters** — host `autoN` pairs with join `auto(N-1)`.
2. **Run length confounds any per-window rate.** Batches with early failures have shorter
   runs and therefore *lower* per-window percentages; the causation runs backwards. Compare
   full-length runs only.

---

## 7. Experimental discipline this rig has taught, the hard way

- **Interleave arms; never block them.** Two false conclusions came from comparing a batch
  run at 01:00 against one run at 15:00. Alternate run by run.
- **State the falsifier before the run**, in the ini comment. It is much harder to rescue a
  dead hypothesis afterwards if you wrote down what would kill it.
- **One variable per arm.** Changing the rate and a code knob together makes both
  uninterpretable.
- **A proxy that disagrees with the outcome is usually the broken one.** The gate-saturation
  metric said 55.00 beat 57.00; trades and seat failures said the opposite. The proxy was
  wrong (unbounded queue model).
- **Verify instruments behaviourally.** Three separate probes on this project were silently
  measuring nothing or the wrong thing — `rfu_answer_census` (missing frontend clock),
  `rfu_rxburst` (counts polls, not deliveries), `rfu_rxgate peak` (masked away by a 12-bit
  trace field). Each looked fine and produced confident numbers.
- **Every run costs a human.** Prefer changes that make one run answer more than one
  question.
