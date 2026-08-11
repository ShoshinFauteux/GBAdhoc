# Gate 4-H — Hardware Acceptance Day

**One scripted session, two real PSPs, 30–45 minutes. You run it once.**

This is the last gate (plan §5 Phase 4 / §7.1 honesty clause): everything else in this
project is proven in emulation, and emulation cannot tell us what a real 2.4 GHz radio
between two consoles in a room does. The session below is written so that **a failure is
as useful as a pass** — every step names the log line that proves it, so even a run that
dies at step 5 sends back a diagnosis rather than "it didn't work".

**Do not debug anything.** If a step fails, write down what you saw, do the "on failure"
note for that step, and keep going to the next independent step. The logs do the analysis.

---

## Before you start

### Kit

- 2 × CFW PSP. Note the model of each (1000 / 2000 / 3000) — model matters for the radio.
- 2 × memory stick with the release installed (`ms0:/PSP/GAME/gpsp-adhoc/`).
- The **same** Pokémon Emerald ROM on both, in `roms/`.
- **Two different save files.** The Union Room refuses two trainers with the same name and
  ID. Use your own save on one console and a distinct-trainer save on the other. Both saves
  should be parked somewhere sane (a Pokémon Center is ideal) with **at least two Pokémon
  in the party you don't mind moving**.
- A pen, or a notes app. There is no on-screen FPS counter — the numbers come from the log.

Name the consoles now and keep the names for the whole session: **A = host**, **B = client**.
Write the model next to each (e.g. "A = PSP-3000, B = PSP-1000").

### Prerequisites — do these on BOTH consoles, and check them twice

| # | Do | Why |
|---|---|---|
| P1 | **Turn the WLAN switch ON.** PSP-1000: **left edge**, below the volume controls. PSP-2000/3000: **top edge, next to the L button**. | Different place per model. If you own both, you will look in the wrong spot on one of them. |
| P2 | **XMB → Settings → Network Settings → Ad Hoc Mode → `Ch 1`** (or 6 or 11 — but the **same on both**, and **never `Automatic`**) | On `Automatic` each console picks its own channel, each creates its own "GPSP07" group, and they are mutually invisible forever with zero error messages. This exact failure ate a full field session. |
| P3 | Charge both, or plug both in | The session is ~40 min and a suspend mid-test invalidates a step |
| P4 | Delete or rename any existing `log/frontend.log` on both sticks | So the log you send back is only this session |

**Prerequisite check line:** after the first launch of the app on each console, the log's
first lines must include `EVT boot_ok` and `EVT clock=333`. If `clock` is not 333, note it
— it changes how every timing number below reads.

---

## The session

Each step: **do**, **PASS looks like**, **the line that proves it**, **on failure**.
All log lines are in `ms0:/PSP/GAME/gpsp-adhoc/log/frontend.log` on the console named.

---

### Step 1 — Solo boot, both consoles (5 min)

**Do:** On each console: XMB → Game → Memory Stick → **PSP AGB** → pick Emerald in the
browser → let the game reach the overworld → walk around for **~2 minutes** → open the menu
(`Select`+`Start` held ~¼ s) → **Exit**.

**PASS:** Both boot to the ROM browser, both load, the game feels full speed, audio is
clean (no crackle, no slow motion), colors look right (the sky is blue, not orange — if red
and blue are swapped, that's a real bug and the run stops here), and the app exits cleanly.

**Proof lines (each console):**

```
EVT boot_ok
EVT clock=333
EVT rom_loaded code=BPEE size=16777216
EVT av_info fps=59.7275 rate=32768 w=240 h=160
EVT audio_rate in=32768 out=44100 step=48695
EVT sram_load size=131072 crc=........
EVT mem_free=...... max_block=......
EVT heartbeat frames=600 t_us=..........
EVT heartbeat frames=1200 t_us=..........
...
EVT exit code=0
```

**The fps number:** there is no on-screen counter, so compute it from two consecutive
heartbeats — they are exactly **600 frames** apart:

> `fps = 600 / ((t_us₂ − t_us₁) / 1 000 000)`

Full speed is **59.73**. A healthy paced run gives **~59–59.8**. Anything at or below ~55
sustained is a performance finding: note which console, and what was on screen.

**Also record:** `mem_free` from each console. The PSP-1000 has the tighter user partition
and this number is the headroom the wireless session has to fit into.

**On failure:** if a console does not boot at all, note the CFW name/version and any error;
that alone is the finding. If `EVT bios=missing` appears, that's fine (built-in BIOS) —
just note it.

---

### Step 2 — Solo save integrity (2 min)

**Do:** Relaunch the app on **both** consoles, load Emerald, check your party/position is
where you left it, then exit.

**PASS:** Both games come back exactly as you left them.

**Proof line:** `EVT sram_load size=131072 crc=........` on the second run has the **same
crc** as the last `EVT sram_flush crc=........ size=131072` of the first run.

**On failure:** stop. Save handling failing solo makes every later save check meaningless.

---

### Step 3 — Bring a session up: host, then join (5 min)

**Do:**
1. On **A**: launch, load Emerald, get to the overworld. Menu → **Wireless** → note the
   **Room** code shown (default `GPSP07`) → **Host session** → **Resume**.
2. Wait ~5 seconds.
3. On **B**: launch, load Emerald, overworld. Menu → **Wireless** → **Join: scan for
   rooms** → wait for the ~10 s scan → pick **A's room code** from the list → **Resume**.
   (If the scan finds nothing, go back and use **Join room code**, D-pad it to the same
   code as A, and press ×.)

**PASS:** Both consoles show a session chip in the corner ("hosting room GPSP07" /
"joined room GPSP07"). Reopening Menu → Wireless on either side shows **WIRELESS – LINKED**
with a Disconnect item.

**Proof lines — A (host):**

```
EVT sav_backup file=..../Emerald.sav.bak size=131072      <- the pre-session backup
EVT adhoc_up group=GPSP07 mac=..:..:..:..:..:..
EVT net_up role=host proto="gpSP v1.0"
EVT session_start id=0 peers=0
EVT mem_free=...... max_block=...... net=up
EVT peer_connected id=1                                    <- B arrived
```

**Proof lines — B (client):** the same, with `EVT net_up role=join` and a
`EVT peer_connected id=0`.

**The one line that decides this step:** `peer_connected` on **both** sides. If it never
appears, look at the host's `EVT net_stats ... peers=0` and `rx=0` — see "Reading the
counters" below; a host with hundreds of `tx` and `rx=0` is the split-channel failure
(prerequisite P2) and the fix is on the XMB, not in the app.

**On failure — WLAN:** if you get "Turn the WLAN switch ON" or the log shows
`EVT net_error reason=wlan_off stage=... rc=... sce=0x........`, that's P1. Fix and redo.

**On failure — no peer:** note it, then **redo this step once** with the roles swapped
(B hosts, A joins). Whether the swap works is itself a valuable datapoint.

---

### Step 4 — Let it idle linked for 60 seconds (2 min)

**Do:** With the session up, just walk around the overworld on both consoles for a full
minute. Don't enter the Union Room yet.

**PASS:** No stutter, no toasts, session chip stays up.

**Proof lines:** `EVT net_stats ...` appears about every 5 s on each console. Check the
**last** one before you move on:

- `overflow=0` — **required**
- `peers=1`
- `retx` should be a small fraction of `acked` (see the reference numbers below)
- no `LOG netdrv: arq overflow` line **anywhere** in the file

**On failure:** if you see `LOG netdrv: arq overflow peer=... (payload lost)` at any point
in this session, **that is the known wireless bug and it has regressed** — copy both logs
and stop; the rest of the run will be noise.

---

### Step 5 — The trade (10 min) — the actual acceptance

**Do:** On both consoles, walk to a **Pokémon Center → upstairs (2F) → talk to the middle
attendant → Union Room**. The two trainers should see each other appear on the map. Greet,
then trade **one Pokémon each way**. Let both games finish their own automatic save
afterwards (they save themselves — do not interrupt).

**PASS:** The trade completes with the normal animation, both games save, both trainers are
back in the Union Room, and the received Pokémon is in your party on both sides.

**Proof lines (both consoles):**

```
EVT net_stats tx=.... rx=.... acked=.... retx=.... dup=....
        drop_crc=0 drop_mal=0 drop_unk=0 overflow=0 drop_dead=0
        core_tx=.... core_rx=.... peers=1
EVT sram_flush crc=........ size=131072       <- the game's own post-trade save
```

`core_tx` and `core_rx` must both be **climbing** across successive `net_stats` lines —
those are the game's own Wireless-Adapter packets going in and out. **`core_tx`/`core_rx`
frozen at a fixed value while `tx`/`rx` keep climbing is the signature failure**: the
transport is alive but the emulated adapter has given up, and the games will show
"communication error" a few seconds later.

**Also note by hand:** roughly how many seconds from entering the Union Room to seeing the
other trainer, and whether anything felt laggy.

**On failure (comm error):** note **which console showed the error first** and roughly how
many seconds after linking. Then read that console's last `net_stats` and record
`retx`, `acked`, `dup`, `rx`, `overflow`. Those five numbers are the whole diagnosis.

---

### Step 6 — Save integrity after the trade (3 min)

**Do:** Exit both apps (Menu → Exit). Relaunch both, load Emerald, check the party.

**PASS:** The traded Pokémon are present on both consoles and nothing else moved.

**Proof line:** `EVT sram_load crc=........` on relaunch equals the last `EVT sram_flush
crc=........` of the trade run, on each console.

**Also:** confirm `Emerald.sav.bak` exists next to `Emerald.sav` in `roms/` on both sticks
(that's the pre-session backup from step 3 — it should hold the *pre-trade* party).

---

### Step 7 — Savestates are blocked during a session (2 min)

**Do:** Bring a session up again (step 3, short version — Host on A, Join room code on B).
With the session live, on A: Menu → **Save state** (×). Then Menu → **Load state** (×).

**PASS:** Both are refused with the toast **"Savestates locked during wireless session"**.
Nothing is written.

**Proof:** **no** `EVT state_save` and **no** `EVT state_load` line appears in A's log
while the session is up. (For contrast: solo, `Save state` logs
`EVT state_save file=... size=... crc=...`.)

**Why this matters:** the emulator core does not serialise Wireless-Adapter state into
savestates, so a mid-session state load would desync the emulated adapter permanently. If
this block is *not* enforced, that is a shipping blocker — note it loudly.

Leave the session up for step 8.

---

### Step 8 — Walk out of range (5 min)

**Do:** With the session live and both games in the overworld (not mid-trade), take **B**
and walk away — out of the room, around a corner, 15–20 m, whatever it takes. Wait until
something changes on screen (up to ~30 s). Then walk back.

**PASS:** Neither console crashes, freezes, or reboots. Expect the game's own
communication-error dialog on one or both sides, and/or a "Wireless group lost" toast. The
app stays responsive; you can open the menu and exit cleanly.

**Proof lines (on the console that noticed):**

```
EVT peer_disconnected id=.            <- our transport declared the peer dead
EVT adhoc_group_lost                  <- (only if the radio group itself dissolved)
EVT net_stats ... drop_dead=..        <- frames aimed at a peer that stopped answering
EVT exit code=0                       <- and it still exits cleanly afterwards
```

`drop_dead` **is expected to be non-zero here** and is not a bug — it is the driver
correctly classifying traffic aimed at a peer that is gone. `overflow` must **still be 0**.

**Record:** roughly how many seconds from "out of range" to the game reacting, and whether
walking back in did anything (re-linking without re-hosting is *not* expected to work).

**On failure:** a crash, a black screen, or a console that needs a battery pull is a major
finding — note exactly what was on screen.

---

### Step 9 — WLAN switch flipped mid-session (5 min)

**Do:** Bring a fresh session up (A hosts, B joins). Both in the overworld. Now **flip B's
WLAN switch OFF** while the game runs. Wait ~30 s. Then flip it back ON and wait ~30 s.

**PASS:** Neither console crashes. B may show a warning/toast and the game its own
communication error; A behaves like step 8. Both remain able to open the menu and exit.

**Proof lines — B:** an `EVT adhoc_group_lost` and/or `EVT net_error reason=wlan_off ...`,
then `EVT exit code=0` when you quit.
**Proof lines — A:** `EVT peer_disconnected id=...`, `overflow=0` in the final `net_stats`.

**Expectation to record honestly:** flipping the switch back ON is **not** expected to
restore the session. What we're testing is that the console survives losing its radio
mid-flight. Note whether anything unexpected recovered.

---

### Step 10 — Host quits, then client quits (4 min)

**Do:**
1. Fresh session (A hosts, B joins), both in the overworld.
2. On **A**: Menu → **Exit**. Watch **B**.
3. Wait 30 s. Then on **B**: Menu → **Exit**.

**PASS:** A exits cleanly and its save is flushed. B survives the host vanishing — the game
shows its own communication error, the app does not crash — and B then exits cleanly too.

**Proof lines — A:** `EVT net_down`, `EVT sram_flush ...`, `EVT exit code=0`.
**Proof lines — B:** `EVT peer_disconnected id=0`, then later `EVT net_down`,
`EVT sram_flush ...`, `EVT exit code=0`, with `overflow=0` in its last `net_stats`.

Then repeat once with the roles reversed (**client quits first**, host must survive):
same expectations mirrored — the host should log `EVT peer_disconnected id=1` and keep
running.

---

### Step 11 — Suspend / resume (3 min) — exploratory, no pass criteria

**Do:** Bring a session up. On **B**, close the lid / hit the POWER switch to **suspend**.
Wait ~20 s. **Resume**. Observe for ~30 s. Then try to exit the app cleanly on both.

**Honest framing:** the frontend calls `scePowerTick` during a session specifically to keep
the console from auto-suspending, but there is **no suspend/resume handler** — the app does
not tear the session down on suspend and does not rebuild it on resume. This step exists to
find out what actually happens, not to confirm a behaviour we designed.

**Record:** does B come back at all? Does the game keep running? Does A notice? Does either
console lock up or need a battery pull? Can both still exit cleanly (`EVT exit code=0`)?
Anything here is a legitimate result; write down what you saw.

---

### Step 12 — Final save check (2 min)

**Do:** Relaunch both, load Emerald, confirm the games are intact and nothing is corrupted
or reverted further back than the last thing you did.

**Proof line:** `EVT sram_load size=131072 crc=........` matches the last `sram_flush` on
each console. Confirm the `.sav.bak` files are still there.

---

## Reading the counters (what "healthy" looks like)

Every ~5 s each console logs:

```
EVT net_stats tx=<datagrams sent> rx=<datagrams received> acked=<payloads confirmed>
     retx=<retransmissions> dup=<duplicates received>
     drop_crc=0 drop_mal=0 drop_unk=0 overflow=0 drop_dead=0
     core_tx=<game packets out> core_rx=<game packets in> peers=<n>
```

and every ~10 s the radio layer logs:

```
EVT adhoc_stats tx=.. txwb=.. txfail=.. rx=.. ringdrop=.. oversize=..
     rxerr=.. ctlevt=.. ctldisc=.. ctlerr=..
```

Known-good reference numbers:

| Signal | Healthy | Meaning if it isn't |
|---|---|---|
| Emulated **fps** (from heartbeats) | **~59** of 59.73, paced | Below ~55 sustained = performance finding |
| **`overflow`** | **0**, always | Any non-zero value, or **any** `LOG netdrv: arq overflow peer=... (payload lost)` line, means the RELIABLE-delivery bug has regressed. This is the single most important line in the file. |
| **`retx` / `acked`** | **well under 0.5** | The reference automated trade ran ≈ **0.11**. A field run that failed measured **≈ 2.8** — every delivered packet took ~3.8 transmissions. Anything above ~1 is a retransmit storm. |
| **`dup` / `rx`** | small | The failed field run hit **~68 %** — two thirds of everything received was a duplicate we'd already been sent |
| `drop_crc` / `drop_mal` / `drop_unk` | **0** | Corruption or framing bugs — genuinely unexpected |
| `drop_dead` | 0 normally; **non-zero is fine and expected** in steps 8–10 | Traffic aimed at a peer that stopped answering; not a defect |
| `txfail`, `rxerr`, `ringdrop`, `ctlerr` (adhoc_stats) | **0** | Real radio/driver errors |
| `core_tx` / `core_rx` | **both climbing** while linked | **Frozen while `tx`/`rx` keep climbing = the emulated adapter timed out** — the transport is alive but the game's link is dead. This is the exact signature of the failure this gate is checking for. |
| `peers` | 1 while linked | 0 with high `tx` and `rx=0` = split ad-hoc channel (prerequisite P2) |

---

## How to send the results back

1. Connect each PSP by USB (USB mode) or pull the memory stick.
2. Copy **the whole log folder from BOTH consoles**:

   ```
   ms0:/PSP/GAME/gpsp-adhoc/log/frontend.log
   ```

   Rename them so they can't be confused: **`frontend-A-host.log`** and
   **`frontend-B-client.log`**. If a console was re-run several times the file is appended
   to across runs — send the whole thing, don't trim it.

3. Also send, in a message or a text file:
   - **Console models** (A = ?, B = ?) and CFW name/version on each.
   - **Ad-hoc channel** you set, and confirmation both were the same.
   - Your **fps numbers** from step 1 (both consoles) and `mem_free` from both.
   - For each step 1–12: **pass / fail / didn't get there**, plus one line of what you saw.
   - For any failure: **which console failed first**, roughly how long after linking, and
     what was on screen.
   - Anything that felt wrong even if it technically passed. Your read on "did it feel like
     a real trade?" is a real datapoint.

4. Optional but great: a phone photo or short video of anything visually wrong (colors,
   garbled screen, an error dialog).

**The two logs are the deliverable.** Even a run that dies at step 3 sends back enough to
tell us whether it was the channel, the switch, the radio, or our driver — that's the whole
point of the design above.
