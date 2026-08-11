# PSP AGB — gpSP-AdHoc

A **native PSP frontend for the modern gpSP GBA emulator core**, with **GBA Wireless
Adapter multiplayer carried over the PSP's own ad-hoc WiFi**.

Put it on two CFW PSPs, load Pokémon Emerald (or FireRed/LeafGreen) on both, walk into
the Union Room, and trade — the games think they are talking to real AGB-015 Wireless
Adapters, and the radio underneath is your PSPs talking directly to each other. No
router, no internet, no RetroArch, no lobby server.

It also just plays GBA games: ROM browser, scaling modes, fast-forward, savestates,
save auto-backup, and an in-game menu.

---

## Credits — this project stands on other people's work

**Everything that makes wireless multiplayer possible was built by other people. We wrote
a PSP shell and a radio transport around it.**

- **Meat Puppet** — hardware test operator. Every number in this repository that describes
  a real PSP came off two consoles he sat in front of, one run at a time: plug in, launch,
  walk into the Union Room, trade, walk out, unplug, swap consoles, repeat. He ran the
  builds that were going to fail as willingly as the ones that were going to pass, and
  reported what he actually saw rather than what the run was supposed to prove — which is
  the whole reason the logs can be trusted.

  He also caught, repeatedly, reasoning that had run ahead of the evidence: a 100 ms
  GBA-internal deadline being compared against a network round-trip, a "double the frame
  rate, double the data" claim the counters flatly contradicted, and gate tests that
  passed while exercising nothing. Two of the load-bearing insights are his outright —
  that the emulated adapter was sending *too much* rather than too little, and that a
  console-to-console frame-rate negotiation protocol was over-engineering. The log
  summariser is his specification, including the part that matters most: that it stay
  non-destructive, so the raw log is always one command away.

- **David Guillen Fandos (davidgfnet)** — maintainer and principal author of the modern
  gpSP core. He **reverse-engineered the GBA Wireless Adapter and implemented its
  emulation** (`rfu.c`, `serial_proto.c`), which is the single hardest and most valuable
  piece of this entire stack. His write-up:
  <https://www.davidgf.net/2024/01/13/gba-wireless-adapter/>. Repo:
  <https://github.com/davidgfnet/gpsp>.
- **Rodrigo Alfonso (afska)** and **Corwin (kuiper.dev)** — the community protocol work on
  the Wireless Adapter that the emulation is built on and cross-checked against:
  [gba-link-connection's `wireless_adapter.md`](https://github.com/afska/gba-link-connection/blob/master/docs/wireless_adapter.md)
  and [blog.kuiper.dev/gba-wireless-adapter](https://blog.kuiper.dev/gba-wireless-adapter).
- **libretro/gpsp** — the upstream repository this is forked from
  (<https://github.com/libretro/gpsp>), including its MIPS32 dynarec (which is why a PSP
  can run this at all), the rewritten video renderer, and the bundled open-source BIOS
  replacement. gpSP itself descends from **Exophase**'s original gpSP and **notaz**'s fork.
- **The libretro project** — the `retro_netpacket_callback` interface (env call 78) that
  lets a frontend carry a core's link traffic over any transport it likes.
- **pret** (`pokeemerald` / `pokefirered` decompilations) — used to understand game-side
  RFU behaviour while debugging.
- **PSPSDK / pspdev** — the toolchain and the `sceNetAdhoc` documentation-by-source.

Licensed **GPL-2.0**, same as gpSP (see `COPYING`). Upstream copyright headers are intact.
Changes to the core itself are kept minimal and are intended to be offered upstream (see
`docs/DECISIONS.md`, ADR-0011 and ADR-0013).

---

## Requirements

- **Two PSPs** for multiplayer (any mix of PSP-1000/2000/3000), each running **CFW** that
  can launch unsigned homebrew (e.g. ARK-4, PRO, LME).
- A memory stick with free space.
- Your own **legally dumped GBA ROMs** (`.gba`). None are included, and none ever will be.
- **Optional:** a real GBA BIOS dump as `gba_bios.bin` (16 KiB, SHA-1
  `300c20df6731a33952ded8c436f7f186d25d3492`) in the app folder. Without it the core falls
  back to its bundled open-source BIOS replacement, which is fine for these games.
- One PSP alone is enough for single-player.

---

## ⚠ Read this before you try wireless — the two things that actually bite

These are not theoretical. Both of them were hit on real hardware during development, and
both look *exactly* like "the app is broken" when they are really a console setting.

### 1. The physical WLAN switch must be ON — and it is in a different place per model

There is a hardware WiFi switch on the console. If it is off, no software on earth can
bring the radio up; the app will show "Turn the WLAN switch ON" / "WLAN switch is OFF" and
log `EVT net_error reason=wlan_off`.

| Model | Where the WLAN switch is |
|---|---|
| **PSP-1000** (fat) | **Left edge** of the console, below the volume controls |
| **PSP-2000 / PSP-3000** (slim/brite) | **Top edge**, next to the **L** button |

If you are used to a 1000 and pick up a 3000, you will look on the left edge and conclude
the switch is missing. It moved. Check both consoles.

### 2. Both consoles must be on the SAME FIXED ad-hoc channel — not "Automatic"

**This is the big one.** On each PSP:

> **XMB → Settings → Network Settings → Ad Hoc Mode → pick `Ch 1`, `Ch 6`, or `Ch 11`**
> (the same one on both consoles — **not `Automatic`**)

`Automatic` lets each console choose its own channel. Two PSPs on different channels each
happily *create* their own ad-hoc group with the same name and **never see each other** —
no error, no timeout message, just two consoles broadcasting into different radio channels
forever. In the logs this looks like a healthy host with `EVT adhoc_up` and hundreds of
packets sent, `txfail=0`, and `rx=0` / `peers=0` for the whole session.

Set channel 1 on both unless you have a reason not to. (In a crowded 2.4 GHz area, try 6
or 11 — just keep them equal.)

Two smaller ones while you're there: keep both consoles **within a few metres** of each
other, and **start the host first**, then join from the second console.

---

## Install

1. Copy the `PSP` folder from the release zip to the **root of your memory stick**. It
   merges into the existing structure and gives you:

   ```
   ms0:/PSP/GAME/gpsp-adhoc/
       EBOOT.PBP          the app
       roms/              <- put your .gba files here
       saves/             (used by per-game variant builds)
       log/               frontend.log lands here
       README.txt         short version of this file
   ```

2. Put your `.gba` ROMs in `ms0:/PSP/GAME/gpsp-adhoc/roms/`. Existing `.sav` files go
   **next to the ROM** with the same base name (`Emerald.gba` → `Emerald.sav`).
3. Optional: drop `gba_bios.bin` into `ms0:/PSP/GAME/gpsp-adhoc/`.
4. Do the same on **both** consoles, with the **same ROM** on each.
5. XMB → Game → Memory Stick → **PSP AGB**.

---

## Using it

### ROM browser

Boots straight into a list of the `.gba` files in `roms/`. Last played is preselected, and
a ` sav` tag marks ROMs that already have a save file. **D-pad** to move, **×** to start.

### In-game controls

| PSP | Does |
|---|---|
| D-pad | GBA D-pad |
| **○** | GBA **A** |
| **×** | GBA **B** |
| L / R | GBA L / R |
| Start / Select | GBA Start / Select |
| **△** | Cycle video preset: `1x` → `fit` → `fit bilinear` → `stretch` → `stretch bilinear` → … (saved) |
| **□** | **Fast-forward** (hold by default; toggle mode and 1.5×/2×/3×/uncapped in Settings) |
| **Select + Start**, held ~¼ second | Open the in-game menu |

(△ and □ are frontend keys, not GBA buttons — the GBA has no equivalent to spend them on.)

### In-game menu

`Select+Start` (hold) → **Resume · Save state · Load state · Wireless · Settings · Exit**.
**×** selects, **○** goes back.

- **Settings:** Video scale, Video filter, Fast-forward multiplier, FF button hold/toggle,
  Room code. Saved to `config.ini` in the app folder.
- **Exit** flushes your save before quitting. (Quitting via the PSP HOME button also
  flushes the save.)

#### The only two files in the app folder you should ever edit

`config.ini` (below) and, on per-game builds, `variant.ini`. **Nothing else in that folder is
meant for you.** In particular there is no `autopilot.ini` any more: that was an internal test
file that could start a wireless link, pick a ROM and skip the menu on its own, and a stale copy
left on a memory stick did exactly that more than once. The app now ignores any `autopilot.ini`
it finds — it will not delete your file, it just writes `legacy_autopilot_ignored` to the log
and carries on. If you are ever unsure whether something is driving the app automatically,
`log/frontend.log` says so on the first few lines.

#### `config.ini` keys with no menu entry

Edit `config.ini` in the app folder to change these. You should not need to — the
defaults are the right answer on both consoles we have measured.

| key | default | what it does |
|---|---|---|
| `net_frameskip` | `0` | Frameskip policy **while a wireless session is live**. `0` = leave frameskip alone (smooth — sessions cost you no rendered frames). `1` = adaptive: once the console has genuinely fallen behind real time for ~3 seconds, draw every *other* frame until it recovers. `2` = always on for the whole session (what v0.1.0 did; kept so you can feel the difference back-to-back). |
| `net_skip_threshold` | `25` | Audio-buffer fill percentage used by mode `2`. Mode `1` no longer uses it — see below. |
| `net_tx_thread` | `1` | Where the wireless *send* happens. `1` = on its own thread (default), `0` = on the emulation thread (v0.1.0 behaviour), `2` = on its own thread at lower priority, so sends fill the idle time the emulator already spends waiting for the screen. If a session costs you frames, this is the knob to A/B first — the answer depends on your console's Wi-Fi hardware, and only your console can tell us. |
| `net_pace_match` | `1` | **Slow the game down during a wireless session so both consoles agree.** `1` = both consoles run at the fixed rate below (default). `0` = both consoles run as fast as they each can. `2` = the older behaviour, where each console tried to track its partner's speed. See below — this one has a sound you will notice. |
| `blit_mode` | `2` | Where the emulator stages each frame before drawing it. `2` = video RAM (default, and the fastest by a wide margin on real hardware — it saves about 0.7 ms of every frame). `1` and `0` are slower routes kept so we can re-measure if a console ever misbehaves. If your PSP cannot fit the buffer in video RAM it falls back to `0` on its own and says so in the log. |
| `net_session_fps` | `29.97` | The fixed speed used by `net_pace_match = 1`, in frames per second, while a session is live. **Only certain speeds are actually possible** — 59.94, 29.97 and 19.98 — because the console can only hold a whole number of screen refreshes per frame. Whatever you type is rounded to the nearest one and the log tells you what you got. Asking for 40 gets you 29.97. |

**Why the game slows down during a link, and why your music drops in pitch.**
Two real Game Boy Advances both run at 59.7275 frames per second, so their link timing
always agrees. The games count link timeouts in **frames, not seconds** — so if one
console runs 20 % faster than the other, the faster one runs out of patience while the
slower one is still mid-reply, and the trade dies. That is exactly what we measured on
real hardware: joining a session costs about 12 fps versus hosting it, on *both* consoles.

So during a session **both consoles deliberately run at the same fixed speed, 29.97 fps** —
half the normal rate, comfortably below what either console struggles with, so neither one can
fall behind the other. (We first tried 40, and the consoles could not actually hold it: the
emulator can only wait a whole number of screen refreshes per frame, and 40 needs about half
the frames to finish inside a single refresh, which they do not. 29.97 is exactly two
refreshes, so it needs no spare time at all.) Being *equal and steady* matters more than being *fast*, because that is what the
games actually require. Trading is a temporary activity; the moment the session ends, the
emulator glides back to full speed on its own. It eases in and out over a few seconds
rather than lurching.

**Earlier versions did this differently**, by measuring your partner's speed and trying to
match it. That worked, but the measured speed moves around constantly as the game's own
workload changes, so the two consoles spent the whole session chasing each other — and a
*moving* speed is itself something the games do not like. A constant is steadier. If you
want the old behaviour back for comparison, set `net_pace_match = 2`. **Note that the
meaning of `1` changed: it used to select the old matching behaviour and now selects the
fixed rate.**

Because the emulator really is running the game slower, the sound really does play slower —
the pitch glides down with it, like a tape slowing. Roughly a whole octave at 29.97 fps, and only
while a session is live. The alternative was to keep the pitch correct and let the sound
buffer run dry, which is half of every second replaced by silence — a constant crackle.
We chose the smooth, in-tune-with-itself version. **If you would rather have the right pitch
and take your chances on the trade, set `net_pace_match = 0`** and tell us how it goes; the
log line `EVT session_pace …` says exactly what it did.

Why `0` is the default: on real hardware both consoles held full emulation speed for an
entire wireless session, so the old always-on frameskip was throwing away pictures for
nothing — you saw stutter that bought no speed. The log now proves which is happening:
`EVT fps emu=… rendered=… skipped=…` in `log/frontend.log`.

Why mode `1` changed: it used to hand the core an audio-driven "skip if you're behind"
rule, and a console that is behind never catches up, so it skipped **580 of every 600
frames** and still missed 60 fps. A console short of ~1.4x needs half its pictures back,
not 97 % of them, so mode `1` now simply draws every other frame while engaged.

`log/frontend.log` also carries `EVT sess_cost …` roughly every 10 seconds while a
session is live. That one line says where a session's per-frame time went — the send
calls, the receive path, the ARQ timers, the log writes — so a slow session can be
diagnosed from the log instead of guessed at.

### Wireless panel — hosting and joining

Menu → **Wireless**:

- **Host session** — brings the radio up on the current **room code** (`GPSP00`–`GPSP99`,
  default `GPSP07`) and starts hosting immediately. There is no "start" button; hosting
  begins as soon as you pick it.
- **Join: scan for rooms** — a ~10 second scan, then a list of rooms found nearby; pick one
  with **×**.
- **Join room code** — join a specific code directly; **left/right on the D-pad** changes
  the number.

Once linked, the panel becomes **WIRELESS – LINKED** with a status line
(`hosting  room GPSP07` / `joined  room GPSP07`) and a **Disconnect** item, and a small
session chip stays in the corner of the screen during play.

Then just play the game's own wireless feature: in Pokémon gen 3 that is the **Pokémon
Center 2F Union Room** (or the Wireless Club). From the game's point of view nothing is
unusual — it discovers a partner, you greet, you trade.

**Both consoles need the same room code** (or use scan-and-join, which picks it for you).

### Fast-forward is locked to 1× during a wireless session

Pressing □ while linked shows "Fast-forward locked: wireless session". This is deliberate:
the emulated Wireless Adapter protocol tolerates latency, but the *games'* link timeouts
are written against real time. A console running 3× faster than its partner distorts that
timing in ways nobody has characterised, and the failure mode is a corrupted trade rather
than a dropped frame. Fast-forward comes back the moment the session ends.

---

## Saves

- Cartridge saves live in **`<rom name>.sav` next to the ROM** — the same 128 KiB format
  RetroArch/VBA/mGBA use for these games, so your existing saves work and can go back.
- Saves are written on exit, when you open the menu, at session start/end, and about every
  5 seconds *if the save actually changed* (the frontend CRCs the buffer rather than
  writing constantly to your memory stick).
- **Every wireless session start copies `<rom>.sav` → `<rom>.sav.bak` first.** Trading is
  the one thing you cannot afford to have corrupted, so there is always a pre-session copy
  one file-rename away.
- **Savestates** (one slot per ROM, `<rom>.st0`) are allowed in single player and are
  **blocked — both save and load — while a wireless session is active**. The emulator core
  does not serialise Wireless Adapter state into savestates, so loading one mid-session
  would leave the emulated adapter's state machine and your partner's console permanently
  out of sync. You get a "Savestates locked during wireless session" toast instead.

---

## What to expect from which games

Wireless Adapter emulation quality varies by game — this comes from the core, not from
this frontend. From the core maintainer's own compatibility notes:

| | Games |
|---|---|
| **Works well** | **Pokémon FireRed / LeafGreen / Emerald**, Mario Golf: Advance Tour, Mega Man Battle Network |
| **Works, but latency-sensitive / janky** | Digimon Racing, Shrek SuperSlam, Mario Tennis: Power Tour |
| **Known broken** | Hamtaro: Ham-Ham Games, Nonono Puzzle Chalien |

Pokémon gen 3 is the acceptance target for this project and the only combination that has
been driven end-to-end automatically (Emerald ↔ Emerald Union Room trade). The
latency-sensitive titles were designed around a link that answers in a millisecond; ad-hoc
WiFi over two consoles in a room is slower than that, and you should expect them to feel
worse than they do on real hardware. Different games *can* link with each other where the
originals did (e.g. FireRed ↔ Emerald) — nothing here assumes both sides run the same ROM.

---

## Status and known limitations

Read `CHANGELOG.md` for the honest version-by-version list. Summary for v0.1.0:

- Single-player on real hardware is solid: Pokémon Emerald runs at ~59 fps of 59.73 at
  333 MHz on a CFW PSP.
- The full trade path (session bring-up, Union Room discovery, trade, both games saving,
  peer-disconnect recovery) is validated automatically, including under injected packet
  loss and jitter — but on emulated PSPs and desktop builds, not yet as a signed-off
  two-console hardware run. That run is `docs/HARDWARE-ACCEPTANCE.md`.
- Wireless session stability on **real radio** is the active work item — see CHANGELOG.
- No link-**cable** emulation, no internet play, no infrastructure WiFi. Out of scope.
- Max 5 players by protocol; only 2 has been exercised.

---

## Building from source

You need Docker (for the PSP toolchain) and a checkout of this repo.

```bash
# 1. Build the gpSP core as a PSP static library
docker run --rm -v "$PWD":/build -w /build pspdev/pspdev \
    sh -c 'make platform=psp1 clean && make platform=psp1'

# 2. Build the frontend + pack the EBOOT
docker run --rm -v "$PWD":/build -w /build/psp pspdev/pspdev make

# -> psp/EBOOT.PBP
```

Package a release zip with `tools/make_release.sh psp/EBOOT.PBP` (see `docs/RELEASE.md`).

**Desktop dev build** (Linux/WSL — a development twin of the same frontend, used by the
test harness; not a supported end-user product):

```bash
sudo apt install libsdl2-dev            # plus build-essential
make -C sdl                             # builds the core as a static archive, then gpsp_sdl
./sdl/gpsp_sdl --rom ROM.gba            # --host / --join drive the same net stack over UDP
```

The automated end-to-end tests live in `tools/e2e/` and run against PPSSPP; see
`docs/TESTING.md`.

---

## Per-game "invisible" builds

You can build a variant that looks like a native PSP port of one specific game — its own
XMB title and icon, boots straight into the ROM, no emulator UI at any point, and wireless
that brings itself up silently the moment the game touches its wireless features. Trades
"just work" with no menus on either console.

```bash
tools/make_variant.sh "Pokemon Emerald.gba" cover.png --title "Pokemon Emerald"
```

See **`docs/VARIANTS.md`** for the options, the runtime `variant.ini` keys, and the
silent-wireless policy. (The two gotchas above still apply — a silent build cannot turn
your WLAN switch on or fix your ad-hoc channel.)

---

## Documentation map

| File | What's in it |
|---|---|
| `docs/HARDWARE-ACCEPTANCE.md` | The scripted two-console hardware test session |
| `docs/VARIANTS.md` | Per-game invisible builds |
| `docs/DECISIONS.md` | Every architectural decision and why (ADRs) |
| `docs/ARCHITECTURE.md` | How the net driver and frontend fit together |
| `docs/TESTING.md` | The automated harness |
| `docs/SERIAL-PROTO-NOTES.md` | Notes on the core's RFU/serial implementation |
| `docs/RELEASE.md` | Release runbook |
| `docs/HW-BASELINE.md` | Measured two-console numbers, per build |

---

## License

GPL-2.0-or-later, inherited from gpSP. See `COPYING`. The bundled 8×16 console font is the
Linux kernel VGA font (GPL-2.0). No ROMs, BIOS images, or save files are distributed with
this project, and the release packaging script refuses to build a zip that contains any.
