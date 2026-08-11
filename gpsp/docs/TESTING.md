# TESTING.md — Living test strategy (plan §7)

This document is the living version of the project's testing strategy (mission brief §7). It records the
verified recipes the autonomous validation harness is built on. The harness scripts themselves land in
`tools/e2e/` (`setup_ppsspp.sh`, `run_boot_test.sh`, `run_nettest.sh`, `run_trade_test.sh`,
`run_soak.sh`, `run_disconnect_matrix.sh`); this file is the specification they implement and the place
where every rig-level fact is either **verified against PPSSPP source** or explicitly flagged as needing a
live-run confirmation. All PPSSPP citations below are against `hrydgard/ppsspp` master @ `d90fdee8`
(2026-07-30), source at `~/ppsspp` in WSL Ubuntu, binaries at `~/ppsspp/build/PPSSPPSDL` and
`~/ppsspp/build/PPSSPPHeadless`.

---

## PPSSPP multi-instance recipe

### 0. The three mechanisms that make N instances work (read this first)

1. **Per-instance sandbox = `XDG_CONFIG_HOME`.** On Linux the memstick directory is
   `$XDG_CONFIG_HOME/ppsspp` (fallback `~/.config/ppsspp`) — `UI/NativeApp.cpp:541-549`. The config ini
   is searched in `<memstick>/PSP/SYSTEM/` (`Core/Config.cpp:1255-1265` via `FindConfigFile`,
   `Core/Util/PathUtil.cpp:13-33`). So **one env var isolates everything**: config, controls, memstick,
   saves, savestates. There is no `PPSSPP_CONFIG` env var; there are `--config=FILE` /
   `--controlconfig=FILE` / `--appendconfig=FILE` flags (`Core/CmdLine.cpp:284-285,410-413,173`;
   appended config applied after main load, `UI/NativeApp.cpp:623-626`) — useful for layering a shared
   base ini, but not required when each instance has its own `XDG_CONFIG_HOME`.

2. **The shared-memory instance counter.** There is **no single-instance lock** — PPSSPP is explicitly
   multi-instance-aware. Each process bumps a counter in POSIX shm `/PPSSPP_ID`
   (`/dev/shm/PPSSPP_ID` on Linux) and gets `PPSSPP_ID = 1, 2, 3...` (`Core/Instance.cpp:54,120-166`,
   called from `Config::Load`, `Core/Config.cpp:1332-1335`). This ID drives:
   - **Loopback IP:** each instance uses `127.0.0.(ID)` — literally `0x7F000001 + PPSSPP_ID - 1`
     (`Core/HLE/sceNet.cpp:517-523`), and when the adhoc server is local, all adhoc sockets bind that
     per-instance IP instead of `INADDR_ANY` (`Core/HLE/sceNetAdhoc.cpp:2139-2142`,
     `Core/HLE/proAdhoc.cpp:2208-2214`). This is what prevents port collisions between instances.
   - **MAC address:** instance 1 uses the `MacAddress` ini key; instances with `PPSSPP_ID > 1`
     **ignore the ini** and get MAC `{ID,ID,ID,ID,ID,ID} & fc:ff:...` (`Core/HLE/proAdhoc.cpp:1941-1945`,
     `Core/HLE/sceNet.cpp:1035-1038`). Distinct MACs per instance are therefore automatic.
   - Audio: instances with ID > 1 are auto-muted (`Core/Config.cpp:1606-1610`).

   Caveats: a crashed run leaves the shm file behind (IDs keep incrementing — harmless), and any
   instance's clean exit `shm_unlink`s it (`Core/Instance.cpp:185-188`), so an instance launched *after*
   another one exited can restart at ID 1 while ID 1's IP/MAC is still in use. **Harness rule: `rm -f
   /dev/shm/PPSSPP_ID` before each run, launch all instances up front, in a fixed order.**

3. **The `localhost` trigger.** `isLocalServer` (which enables the per-instance loopback binding above) is
   set iff `proAdhocServer` is `localhost` or starts with `127.` (`Core/HLE/sceNet.cpp:525-526`). Getting
   this key right is load-bearing, not cosmetic.

### 1. ini keys (exact names, exact sections)

Section names from `Core/Config.cpp:1175-1181`; network keys `Core/Config.cpp:1063-1092`; system-param
keys `Core/Config.cpp:1094-1113`.

| Section | Key | Value for harness | Source / notes |
|---|---|---|---|
| `[Network]` | `EnableWlan` | `True` | default False (`Config.cpp:1064`); acts as the WLAN switch — `sceWlanGetSwitchState` returns it (`sceNet.cpp:1060-1064`) |
| `[Network]` | `EnableAdhocServer` | `True` on instance 1 only | spawns built-in server thread at game boot when `EnableWlan` is also true (`sceNetAdhoc.cpp:1913-1915`) |
| `[Network]` | `proAdhocServer` | `localhost` | default is public `socom.cc` (`Config.cpp:821-823`); `localhost`/`127.*` triggers multi-instance loopback mode (`sceNet.cpp:525-526`) |
| `[Network]` | `PortOffset` | `10000` (default, same on all instances) | real port = PSP port + offset (`sceNetAdhoc.cpp:2143`). With a local server each instance binds its own `127.0.0.x`, so the offset does NOT need to differ per instance; it exists to keep real ports out of the privileged (<1024) range that PSP games use (e.g. adhoc port 0x309/777) |
| `[Network]` | `EnableUPnP` | `False` (default) | determinism; `Config.cpp:1073` |
| `[Network]` | `AdhocServerRelayMode` | `2` (AlwaysOff) | enum Auto=0/AlwaysOn=1/AlwaysOff=2 (`ConfigValues.h:84-88`); Auto consults a cached server list (`sceNetAdhoc.cpp:1929-1949`) — pin it off |
| `[SystemParam]` | `MacAddress` | e.g. `02:00:00:00:00:01` | only honored by instance with `PPSSPP_ID == 1`; regenerated randomly if malformed (`Config.cpp:1598-1600`, `CreateRandMAC` `Config.cpp:152-170`). IDs > 1 auto-derive their MAC (see §0.2) |
| `[SystemParam]` | `NickName` | `INST1` / `INST2` / ... | shows in adhoc server logs; aids debugging |
| `[General]` | `FirstRun` | `False` | skip first-run UI flow |
| `[General]` | `CheckForNewVersion` | `False` | no network noise (`Config.cpp:240`) |
| `[General]` | `DiscordRichPresence` | `False` (default) | `Config.cpp:243` |

**Shared-MAC failure mode (verified):** the built-in server only *warns* on a duplicate MAC login
(`proAdhocServer.cpp:573-579`) but games identify peers by MAC, so pairing breaks silently. A duplicate
*IP* login is refused — the server closes the stream (`proAdhocServer.cpp:500-506,549-550`). This is
exactly why the per-instance `127.0.0.x` IPs and per-ID MACs matter.

### 2. AdhocServer topology (verified from source)

The built-in server (`Core/HLE/proAdhocServer.cpp`) binds **`INADDR_ANY:27312`**
(`proAdhocServer.cpp:1824-1831`; `SERVER_PORT` = 27312, `Core/HLE/proAdhoc.h:44`) and is a plain TCP
matchmaking hub — it happily serves any number of local clients. It is started as a thread inside the
emulator at PSP boot iff `EnableWlan && EnableAdhocServer` (`sceNetAdhoc.cpp:1903-1916`), and it
**skips starting if something is already listening** on `proAdhocServer:27312`
(`proAdhocServer.cpp:1668-1671`). Clients connect to `proAdhocServer` resolved + port 27312
(`proAdhoc.cpp:1380-1396`), each from its own `127.0.0.x` source IP.

**Recommended topology (cleanest to script):** instance 1 hosts the server (`EnableAdhocServer=True`
in its ini only), all instances point `proAdhocServer=localhost`. Launch instance 1 first, wait until TCP
127.0.0.1:27312 accepts, then launch the rest. (Enabling it on every instance also works thanks to the
already-listening check, but that's a race we don't need.) PPSSPP has no separate standalone
AdhocServer binary target, and none is needed.

### 3. Directory layout for N sandboxes

**Sandboxes must live on a native Linux filesystem** (default `~/gpsp-e2e/sandboxes`, override with
`SANDBOX_ROOT`). With the memstick on a WSL `/mnt/c` (DrvFs) mount the boot fails outright — verified
live 2026-07-31. The repo checkout may stay on `/mnt/c`; only the sandboxes (and PPSSPP itself) need
native FS.

```
~/gpsp-e2e/sandboxes/
├── inst1/                      # XDG_CONFIG_HOME for instance 1
│   └── ppsspp/                 # = memstick root (ms0:)  [NativeApp.cpp:549]
│       └── PSP/
│           ├── SYSTEM/
│           │   └── ppsspp.ini          # seeded by setup script (EnableAdhocServer=True here)
│           └── GAME/
│               └── gpsp-adhoc/
│                   ├── EBOOT.PBP       # our homebrew
│                   ├── .gpsp-harness.ini  # harness control channel (ADR-0036;
│                   │                    #   was autopilot.ini, renamed so a user
│                   │                    #   file can never trigger it)
│                   ├── roms/ saves/    # fixtures (copied in per-run)
│                   └── log/            # structured EVT logs + BMP screenshots (our I/O channel)
├── inst2/                      # same shape; ppsspp.ini has EnableAdhocServer=False
└── inst3/                      # optional third player
```

Why `PSP/GAME/gpsp-adhoc/`: `ms0:` maps to the memstick dir (`Core/HLE/sceIo.cpp:661-672`), and when
the EBOOT's host path contains `PSP/GAME/`, PPSSPP sets the homebrew's working directory to the true
`ms0:/PSP/GAME/<dir>/` path (`Core/PSPLoaders.cpp:366-374`; the EBOOT's real directory is also mounted
as `umd0:`, `PSPLoaders.cpp:410-411`). Placing the EBOOT anywhere else degrades to `umd0:/` semantics.
Everything the app writes under `ms0:` is directly visible to the host in `instN/ppsspp/` — this is the
harness's log/screenshot/fixture channel.

### 4. Minimal working ppsspp.ini (instance 1; diff noted for others)

```ini
[General]
FirstRun = False
CheckForNewVersion = False

[Graphics]
GraphicsBackend = 3 (VULKAN)    ; REQUIRED under Xvfb: OpenGL fails at first boot (no matching
                                ; GLX visual for PPSSPP's requested config) and PPSSPP only recovers
                                ; via crash-marker + relaunch; Vulkan-on-lavapipe works first try.
                                ; Verified live 2026-07-31.

[Network]
EnableWlan = True
EnableAdhocServer = True        ; instance 1 only — False in inst2..N
proAdhocServer = localhost
AdhocServerRelayMode = 2
PortOffset = 10000
EnableUPnP = False

[SystemParam]
NickName = INST1                ; INST2, INST3 ... per instance
MacAddress = 02:00:00:00:00:01  ; honored only when PPSSPP_ID==1; harmless elsewhere
```

All other keys take defaults (and PPSSPP will rewrite/expand this file on exit — that's fine; sandboxes
are recreated per run). Note ini booleans are `True`/`False`.

### 5. Launch command lines — 2-instance adhoc session under xvfb

```bash
#!/usr/bin/env bash
set -euo pipefail
PPSSPP="$HOME/ppsspp/build/PPSSPPSDL"          # SDL3 build; run from build dir so assets/ resolves
SB="$(pwd)/tools/e2e/sandboxes"
ART="$(pwd)/tools/e2e/artifacts/$(date +%s)"; mkdir -p "$ART"

# 1. Clean slate: instance counter must start fresh (see §0.2 caveats)
rm -f /dev/shm/PPSSPP_ID

# 2. One virtual display is enough for both windows
Xvfb :99 -screen 0 1280x720x24 &
XVFB_PID=$!
export DISPLAY=:99
export SDL_AUDIODRIVER=dummy                   # no audio device in CI

# 3. Instance 1 (hosts the built-in AdhocServer). Order matters: first launch => PPSSPP_ID=1.
XDG_CONFIG_HOME="$SB/inst1" "$PPSSPP" \
    --windowed --escape-exit --graphics=software \
    --log="$ART/inst1.emu.log" \
    "$SB/inst1/ppsspp/PSP/GAME/gpsp-adhoc/EBOOT.PBP" &
PID1=$!

# 4. Wait for the built-in AdhocServer before starting the client
until nc -z 127.0.0.1 27312; do sleep 0.2; done

# 5. Instance 2 (client). PPSSPP_ID=2 => IP 127.0.0.2, MAC 02:02:02:02:02:02 automatically.
XDG_CONFIG_HOME="$SB/inst2" "$PPSSPP" \
    --windowed --escape-exit --graphics=software \
    --log="$ART/inst2.emu.log" \
    "$SB/inst2/ppsspp/PSP/GAME/gpsp-adhoc/EBOOT.PBP" &
PID2=$!

# 6. ...harness now polls $SB/instN/ppsspp/PSP/GAME/gpsp-adhoc/log/*.log for EVT markers,
#    with timeouts; collects logs+screenshots into $ART; kills PIDs; kills Xvfb.
```

Relevant CLI facts (all from `Core/CmdLine.cpp` unless noted):
- **Positional file argument boots it directly** and skips the browser UI entirely
  (`UI/NativeApp.cpp:630-658`, `skipLogo = true`).
- `--windowed` / `--fullscreen` (`CmdLine.cpp:168-169`, applied `CmdLine.cpp:431-434`).
- `--escape-exit` (ESC quits — clean teardown), `--pause-menu-exit` (`CmdLine.cpp:171-172`).
- `--state=FILE` loads a savestate at boot when a boot file is given (`UI/NativeApp.cpp:711-713`) —
  this is how fixture savestates park a run next to the Union Room.
- `--log=FILE`, `--loglevel=N` (`UI/NativeApp.cpp:612-615`).
- `--screenshot=FILE` is **headless-only** in effect (consumed only in `headless/Headless.cpp:644-645`);
  for the SDL app, screenshots come from *our homebrew's* BMP dumps to `ms0:` — by design, per plan §7.1.
- `--appendconfig=FILE` merges an extra ini over the loaded config (`UI/NativeApp.cpp:623-626`).

### 6. PPSSPPHeadless verdict: NOT usable for the adhoc e2e

`PPSSPPHeadless` contains the full HLE net stack and even force-enables WLAN with a fixed MAC
(`headless/Headless.cpp:583-584`), **but**: it never calls `Config::Load` — it runs
`g_Config.RestoreDefaults(...)` plus hardcoded overrides (`Headless.cpp:532,553-591`), so
`--config` is ignored and there is no way to set `proAdhocServer` / `EnableAdhocServer` /
`AdhocServerRelayMode` from outside. Because `InitInstanceCounter()` only runs inside `Config::Load`
(`Core/Config.cpp:1332-1335`), `PPSSPP_ID` stays 0 in headless: every instance computes localhost IP
`127.0.0.0` (`sceNet.cpp:519`) and shares the same hardcoded MAC — the duplicate-IP login refusal
(§1) makes a second instance impossible. `proAdhocServer` stays at its default public `socom.cc`.
**Verdict: the trade e2e runs on `PPSSPPSDL` under Xvfb; `PPSSPPHeadless` is only for solo boot/CPU
smoke tests (`--timeout`, `--screenshot` compare).**

### 7. Build facts

- **SDL3 is required** for the desktop build: CMake hard-fails without `SDL3` + `SDL3_ttf`
  (`CMakeLists.txt:323-341`; Ubuntu: `libsdl3-dev libsdl3-ttf-dev`). `SDL/SDLMain.cpp` includes
  `<SDL3/SDL.h>`.
- Binary paths (confirmed present, built 2026-07-31): `~/ppsspp/build/PPSSPPSDL`,
  `~/ppsspp/build/PPSSPPHeadless`. Run PPSSPPSDL with cwd = build dir (or any dir where
  `assets/flash0` resolves relative to the exe, `UI/NativeApp.cpp:550`).

### 8. Verified vs. assumed

| Fact | Status |
|---|---|
| `XDG_CONFIG_HOME` isolates config+memstick per instance (Linux) | **Verified** (`NativeApp.cpp:541-549`) |
| ini section/key names in §1, defaults, and section headers | **Verified** (`Config.cpp:1063-1113,1175-1181`) |
| Instance counter → per-instance `127.0.0.x` IP + auto MAC for ID>1; no instance lock | **Verified** (`Instance.cpp`, `sceNet.cpp:517-523`, `proAdhoc.cpp:1941-1945`) |
| Built-in server binds `0.0.0.0:27312`, multi-client, skip-if-running | **Verified** (`proAdhocServer.cpp:1668-1671,1824-1831`) |
| Duplicate IP login refused; duplicate MAC only warned | **Verified** (`proAdhocServer.cpp:500-551,573-579`) |
| Same `PortOffset` on all instances is correct when server is localhost | **Verified in code** (per-IP binding, `sceNetAdhoc.cpp:2139-2142`) — confirm with live 2-instance echo test |
| Headless cannot do multi-instance adhoc | **Verified** (`Headless.cpp:532`, `Config.cpp:1332-1335`) |
| Positional EBOOT boot, `--windowed`, `--state`, `--log`, `--appendconfig` | **Verified** (`CmdLine.cpp`, `NativeApp.cpp`) |
| `ms0:` = memstick dir; `PSP/GAME/` path → proper ms0 working dir for homebrew | **Verified** (`sceIo.cpp:661-672`, `PSPLoaders.cpp:366-374`) |
| SDL3 required; binaries at `build/PPSSPPSDL`, `build/PPSSPPHeadless` | **Verified** (`CMakeLists.txt:323-341`, build dir listing) |
| Rendering under Xvfb | **Verified 2026-07-31 @ d90fdee8** — works with Mesa llvmpipe: `LIBGL_ALWAYS_SOFTWARE=1` + `Xvfb -screen 0 1280x720x24 +extension GLX +render` (packages `libgl1-mesa-dri libglx-mesa0`). NOTE: `--graphics=software` alone still needs a GL context to present — without GLX it dies with "Couldn't find matching GLX visual"; use the llvmpipe combo |
| `SDL_AUDIODRIVER=dummy` suffices for audio-less CI | **Verified 2026-07-31** (used in the first successful boot test) |
| Two local instances complete adhocctl group create/join + PDP send/recv end-to-end | **Assumed** — this is exactly what `run_nettest.sh` (transport echo, plan §7.1) exists to prove |
| A PSAR-less homebrew `EBOOT.PBP` takes the `Load_PSP_ELF_PBP` path described above | **Verified 2026-07-31** — `tools/e2e/hello` EBOOT booted in a sandboxed instance (`XDG_CONFIG_HOME` isolation confirmed live) and its `ms0:` log (`EVT boot_ok` / `EVT clock=222` / `EVT exit code=0`) was read back from the host — the full observation channel works |
| Stale `/dev/shm/PPSSPP_ID` handling: `rm -f` pre-run is sufficient | **Verified logic**, live-run confirm alongside boot test |

Anything in the "Assumed" rows is a Phase-0 harness bring-up task; flip each to Verified with the run
that proves it and note the date + PPSSPP commit here.

---

## `SANDBOX_ROOT` DOES NOT ISOLATE THE AD-HOC TESTS (verified 2026-08-03)

**Two agents cannot run ad-hoc e2e tests on this rig at the same time, however they set
`SANDBOX_ROOT`.** `SANDBOX_ROOT` isolates the memstick — config, saves, logs, EBOOT — and nothing
else. Two things the ad-hoc path needs are **global to the machine**:

1. **The matchmaking server port.** `SERVER_PORT` is a compile-time `27312` (`Core/HLE/proAdhoc.h:44`)
   and the built-in server binds `INADDR_ANY:27312` (`proAdhocServer.cpp:1824-1831`). The first
   instance to start owns it; every later instance, in any sandbox, skips starting its own
   (`proAdhocServer.cpp:1668-1671`) and talks to whichever agent's server got there first.
2. **The instance-ID counter,** POSIX shm `/PPSSPP_ID` (`Core/Instance.cpp:54,120-166`), which
   assigns the per-instance loopback IP `127.0.0.(ID)` (`sceNet.cpp:517-523`). Every harness script
   does `rm -f /dev/shm/PPSSPP_ID` before launching, so **each agent resets the other's counter**
   and two instances in different sandboxes end up claiming `127.0.0.1`. A duplicate-IP login is
   refused outright — the server closes the stream (`proAdhocServer.cpp:500-506,549-550`).

**Two symptoms, both caught live on 2026-08-03.** If the other agent's server is up when yours
starts, you get a clean boot then `EVT net_error reason=adhoc_init stage=ctl_connect_wait rc=-9`
and `FAIL: inst1 adhoc bring-up never completed`. If both of your instances DO connect — but to
the other agent's server — they never see each other: the host parks at `EVT ap_mark
text=wait_partner` indefinitely and the run ends `FAIL: host: trade never started`. The second is
the nastier one, because everything up to pairing looks perfectly healthy. Both look exactly like
transport regressions and neither is one. Before believing an ad-hoc failure, check:

```sh
pgrep -af PPSSPPSDL         # anyone else's instances alive?
ss -ltnp | grep 27312       # whose process owns the server?
ps -o pid,cmd -p <that pid> # the sandbox path names the owner
```

That last line is the whole diagnosis: on 2026-08-03 it printed
`./PPSSPPSDL ... /gpsp-e2e/sb-exitroom/inst1/...` while a `sb-perfnext` trade run was sitting at
`wait_partner` — a different agent's instance owning the port my instances were pairing through.

**Which gates this affects.** `run_nettest.sh`, `run_trade_test_psp.sh`, `run_exit_room_test.sh`,
`run_silent_trade.sh`, `run_soak.sh` — anything launching two instances. **Unaffected and safe to
run concurrently:** `run_boot_test.sh`, `run_gu_color_test.sh`, `run_ui_smoke.sh`, `run_save_test.sh`
(single instance), and the netdrv unit tests (no PPSSPP at all).

**Also: kill your own orphans.** A timed-out or failed run leaves `xvfb-run`/`PPSSPPSDL` alive, and
they keep holding 27312. `pkill -f "<your-sandbox-name>"` matches only your own processes because
the sandbox path is on every command line — never blanket-`pkill PPSSPP` on a shared rig.

Real isolation would need a network namespace or a container per agent; the port is not
configurable. Until then, ad-hoc gates are a serialised resource — coordinate, or run them when
the rig is quiet.

## Autopilot + fast-forward harness layer (Phase 1, plan §7.1 step 3)

- **Engine:** `frontend-common/fe_autopilot.[ch]` — input scripts with RAM-predicate sync points
  (grammar in the header). RAM access: EWRAM via `retro_get_memory_data(SYSTEM_RAM)`, IWRAM via the
  `SET_MEMORY_MAPS` descriptors (FRONTEND-AUDIT §6). Verified Emerald BPEE rev-0 address table +
  provenance: `docs/AUTOPILOT.md`. Committed scripts: `testdata/fixtures/*.inputs`.
- **Activation:** desktop twin `--script FILE [--ff]`; PSP frontend `.gpsp-harness.ini` keys
  (**renamed from `autopilot.ini` by ADR-0036** — the file moved, the input-script *engine* is
  still called autopilot and still emits `ap_loaded`/`ap_done`/`ap_fail`)
  `script = <file relative to the app dir>` and `ff = 1`. Script end → `EVT ap_done` + clean exit
  (`EVT exit code=0`); predicate timeout → `EVT ap_fail` + `EVT exit code=3 reason=ap_fail`.
- **Uncapped FF (harness infrastructure — user-facing FF UI is Phase 5A):** no pacing/vblank wait,
  audio muted (rings/queues drain to silence), only every 32nd frame blitted; BMP dumps still work
  (they read the core's frame buffer, not the screen). Measured soak throughput in PPSSPP under
  Xvfb/llvmpipe: ~150 emulated fps (~2.5×) — functional compression only; performance truth stays
  a Gate 4-H question (§7.1 honesty clause).

### E2E drivers (all: exit 0 = pass, artifacts in `tools/e2e/artifacts/<name>-<timestamp>/`)

| Script | What it proves | Key assertions |
|---|---|---|
| `run_boot_test.sh` | boot, BIOS, ROM, SRAM load, video, heartbeats, clean exit | full EVT sequence, frame-600 BMP, `.sav` byte-identical |
| `run_save_test.sh` | in-game SAVE persists across restarts | run 1: RAM-predicate-synced save (start menu → SAVE → overwrite → SRAM CRC change), run 2: `sram_load crc` == run-1 flush crc AND `gSaveBlock1Ptr`-derefed pos/map equal run-1's logged values |
| `run_soak.sh` | 30 min emulated gameplay, no crash / no EVT gap | gapless 600-frame heartbeat ladder ≥ 108000 frames, `clock=333`, script interactivity check at the end, `.sav` untouched, `EVT exit code=0` |

Wall-clock: save test ≈ 4 min; soak ≈ 20–30 min (FF-bound; `TIMEOUT_S` env overrides, default
3600 s). All three run the PSP `EBOOT.PBP` in the §1–§5 single-instance sandbox recipe. The three
scripts share the sandbox directory — run them sequentially, never concurrently.

---

## Gate-3 trade e2e (Phase 3, desktop twin ×2 over netdrv UDP)

The full-stack-minus-PSP-transport acceptance (plan §5 Phase 3). Two `sdl/gpsp_sdl` instances
(RetroArch not involved) complete an autonomous Emerald↔Emerald **Union Room trade** over the
netdrv UDP driver with the fault shim at **5 % loss + 30 ms jitter on both sides**. Runs inside
WSL/Linux; no display needed (`SDL_VIDEODRIVER=dummy`).

### Fixture generation — `tools/e2e/make_trade_fixtures.sh`

Plays the user save (AUSTIN) and a `clone_sav.py` clone (CLONEB, regenerated every run) from the
Route 117 Day Care spot to the Mauville Pokémon Center 2F wireless counter under uncapped FF
(offline, so FF is available) via `testdata/fixtures/emerald_park.inputs`, in-game saves at (6,4)
facing the Union Room attendant, and snapshots `emerald_parkedA.sav`/`emerald_parkedB.sav` into
`testdata/fixtures/` (gitignored, regenerated on demand — DECISIONS user decision 1). ≈ 40 s
per side. Route planned from pret map data with `tools/e2e/map_grid.py` (collision/behavior/NPC
grids); parked parties decoded with `tools/e2e/read_party.py` (full gen-3 substructure decrypt).

### The trade itself — `tools/e2e/run_trade_test.sh`

Emerald's Union Room has **no direct trade menu** — trades go through the **trading board**:
the host (CLONEB, netdrv `--host`, `emerald_trade_host.inputs`) registers its slot-2 Magcargo
requesting DRAGON (the type of the joiner's offer), retreats to a safe tile and waits hands-off;
the joiner (AUSTIN, `--join`, `emerald_trade_join.inputs`) reads the board and offers its slot-1
Salamence. One `mash A … cb2==CB2_LinkTrade` per side rides every prompt (empty-board cycles are
self-retrying); the link trade animation + the game's own automatic in-game save
(`CB2_SaveAndEndWirelessTrade`) then run without input. FF is interlocked off (netdrv session
active), so the run is real-time: ≈ 2.5 min plus fixture time; whole test < 5 min.

**Trade oracle** (asserted on BOTH sides): with `a0` = parkedA slot-1 mon and `b1` = parkedB
slot-2 mon (personality+species from `read_party.py`): RAM — `EVT ap_val` pre/post party
personalities swap exactly (`host.post1 == join.pre0 != host.pre1`, `join.post0 == host.pre1 !=
join.pre0`, other slots untouched); disk — the post-run `.sav` decodes with the received mon in
the traded slot (proves the in-game save flushed the trade); plus `EVT session_start` +
`peer_connected` both sides, post-trade `sram_flush` both sides, final map == Union Room
(0x3C19), clean `EVT exit code=0`, and driver health under impairment: `overflow=0 drop_crc=0
drop_mal=0, acked>0` (the RELIABLE contract held). Screenshots collected from both sides.

**`--disconnect` mode**: host runs `emerald_trade_host_disc.inputs`; the harness SIGKILLs the
joiner 8 s after it logs `ap_mark text=trade_anim`. Asserts the host **survives with the game's
own dialog** ("Communication error… A Button: Registration Counter", `CB2_PrintErrorMessage`,
BMP captured) and exits cleanly — no crash, and the game itself offers re-entry via the counter.

Diagnostic aids added for this work (both permanent): `--trace NAME:SZ:ADDR[:OFF]` on the
desktop twin samples RAM every 300 frames into `EVT trace` lines (how the Union-Room state-pair
wedges were found), and netdrv logs ARQ channel resets and keepalive deaths with silent-ms +
counters. Verified 2026-08-01: normal mode PASS (`artifacts/trade-*`), `--disconnect` PASS
(`artifacts/tradedisc-*`), netsmoke re-PASS, netdrv unit suite green after the ADR-0010 retune.

---

## Phase-4 PSP wireless e2e (PPSSPP ×2, sceNetAdhoc PDP transport)

Both drivers run the §1–§5 two-instance recipe: fresh sandboxes from `setup_ppsspp.sh
--instances 2 --skip-build` (built-in AdhocServer on inst1 only), `rm -f /dev/shm/PPSSPP_ID`,
inst1 launched first. **Launch gating:** the second instance launches only after inst1's EVT log
shows `adhoc_up` (group registered at the AdhocServer) — a bare `nc -z :27312` probe is too
early-biased because a first boot in a fresh sandbox can take >60 s of Vulkan/llvmpipe warmup
before the EBOOT even runs. The wireless surface is .gpsp-harness.ini-driven (`host=1`/`join=1`,
`nick`, `group`, `nettest=1`) until the Phase-4 UI panel lands.

### `run_nettest.sh` — transport echo, no core (bring-up step a)

.gpsp-harness.ini `nettest = 1` makes the EBOOT run the transport echo instead of booting the
core (no ROM in the sandbox at all): full sceNetAdhoc bring-up (module load → sceNetInit →
adhocctl connect "GPSP07" → PdpCreate 0x4A4B → RX thread), then the join side broadcasts 24-byte
`GPNT` pings at 10 Hz, the host unicasts each one back to its source MAC, and both sides pass at
≥ 30 round trips (`EVT nettest_done`, `EVT exit code=0`; joiner tells the host to stop with an
NTEND burst). Asserts: `adhoc_up` with **distinct MACs** (multi-instance isolation), both
`nettest_done` counts ≥ 30, clean exits, `ctlerr=0` in `adhoc_stats`. This flips TESTING §8's
last "Assumed" row (two local instances complete adhocctl create/join + PDP send/recv) to
**Verified** — see the Gate-4E record in DECISIONS.md for the run.

### `run_trade_test_psp.sh` — Gate 4-E trade leg (+ `--disconnect`)

Port of `run_trade_test.sh` to the PSP EBOOT: same parked fixtures (`emerald_parkedA/B.sav`,
regenerated via `make_trade_fixtures.sh` when missing), same trade input scripts (copied into
each instance's `PSP/GAME/gpsp-adhoc/`, referenced by .gpsp-harness.ini `script =`), same oracle.
inst1 = host (CLONEB, `host = 1`), inst2 = join (AUSTIN, `join = 1`). The run asserts the same
driver-health line: `overflow=0 drop_crc=0 drop_mal=0`, `acked>0`, plus a **retx/acked ≤ 60 %**
storm guard. FF is
interlocked off by the session, so the trade runs real-time. Additional PSP-only assertions:
`adhoc_up` both sides and `adhoc_stats` logged. `--disconnect` SIGKILLs the join **PPSSPP
process** 8 s after the joiner logs `ap_mark text=trade_anim` and asserts the host survives with
the game's own communication-error dialog (`host_error_dialog` mark + BMP), `disc_done`, clean
exit — the surviving game's own UI offers wireless re-entry, so the session is re-hostable
without an app restart. In disconnect runs the survivor's `net_stats` shows `drop_dead>0`
(RELIABLE frames the game kept sending to the killed peer — classified separately by netdrv,
ADR-0012 amendment) while `overflow` stays 0. Wall-clock ≈ 8–12 min (two emulator boots +
real-time trade; `TIMEOUT_S` default 900).

#### `--radio=40 | 80 | 160` — emulated ad-hoc radio (added 2026-08-01, issue #2)

**Read this before touching ARQ timing again.** PPSSPP's AdhocServer loopback RTT is
microseconds; real PSP ad-hoc RTT is 40–80 ms. That single gap is why an RTO tuned on the
harness stormed on hardware and cost a field session (docs/HANDOFF.md issue #2). The PSP
transport now carries the same debug fault shim the UDP backend always had
(`adhoc_transport_set_fault`, driven by .gpsp-harness.ini `net_latency_ms` / `net_jitter_ms` /
`net_loss_pct`), applied on the **receive** side — so each instance delays its own RX and two
peers give ~2× the configured latency as RTT. Counters land in `EVT adhoc_stats
faultdel= faultdrop=`; the shim costs one integer test per datagram when unset.

| flag | per-side | jitter | ≈RTT | what it is for |
|---|---|---|---|---|
| `--radio=40` | 20 ms | 10 ms | 40 ms | typical real ad-hoc |
| `--radio=80` | 40 ms | 15 ms | 80 ms | poor radio / power-save |
| `--radio=160` | 80 ms | 20 ms | 160 ms | **queue-stall stress**, not a plausible radio |

`--radio=160` is deliberately beyond the medium: at 160 ms RTT a 16-deep window sustains only
~100 payload/s against the core's ~120/s demand, so the tx queue grows without bound. That is
the harness reproduction of the field's step 3→4 (window stalls awaiting acks → ring backs up →
RELIABLE payload discarded → RFU state destroyed → the game's own communication error).
Artifacts go to `artifacts/psptrader{40,80,160}-<ts>/`.

Measured on the pre-fix build (proof the profiles bite):

| profile | host retx/acked | join retx/acked | dup/rx | overflow | outcome |
|---|---|---|---|---|---|
| `--radio=40` | 148 % | 148 % | 53 % | 0 | trade completed |
| `--radio=80` | 202 % | 211 % | 60 % | 0 | trade completed |
| `--radio=160` | 300 % | 300 % | 63 % | **5342 (join)** | **both games wedged** |

(Field hardware, for calibration: 280 % / 68 % / overflow=2.) In every case
`txfail=0 ringdrop=0 faultdrop=0` — nothing was ever actually lost; the storm was entirely
self-inflicted.

**MEASURED REAL AD-HOC RTT — 2026-08-01, the first field session that completed a trade.**
Two PSPs, real radio, Emerald<->Emerald Union Room trade **completed**. `EVT net_stats` on the
host reported **`srtt_us` 40,500 – 87,000, typically ≈52,000 (≈52 ms)**; the client's log agrees
(44–106 ms, typically ≈51 ms). RTO adapted to ≈205 ms on spikes. Session health:
`retx_pct=2–3 %` (the pre-fix field number was **280 %**), `dup` 1.8 % of rx, `overflow=0`,
`spill=0`, `txfail=0 ringdrop=0 rxerr=0`, and core payload counters mirrored exactly across the
link (host `core_tx=9129 core_rx=17553`, client the reverse) — **zero end-to-end loss**.

Consequences for the profiles above:

- **`--radio=40` is the profile to run** (20 ms/side ⇒ ≈40 ms RTT, closest to the measured
  ≈52 ms). `--radio=80` is a poor-radio case; `--radio=160` remains stress, not a medium.
- **The 100 ms PSP RTO floor (ADR-0017) was the right call and needs no retune.** It sat above
  the true RTT most of the time — which is exactly what a floor is for on a link whose 802.11
  layer already repairs loss beneath us — and the estimator took over on spikes. The floor is
  no longer a judgement call from two broken logs; it is calibrated against a working session.

#### `EVT core_prof` — attributing a core frame-time spike (added 2026-08-02, ADR-0028; split by cause ADR-0029)

```
EVT core_prof core=mean/winmax/max win=rom/full/smc/dma/page wspike=… spike=…
```

Emitted on the **heartbeat cadence, session or not** — deliberately, because the field fact that
motivated it was a 21 ms `retro_run` **before any peer connected**, and `EVT sess_cost` only
exists while a session is live.

- `core` — µs inside `retro_run` itself: mean over this window / worst in this window / worst ever.
- `win` — counter deltas over this window.
- **`wspike` — the counter deltas on the worst frame IN THIS WINDOW. Read this one first.**
- `spike` — the same for the worst frame ever seen. **It usually latches during boot**, so all
  zeros here say nothing about what is happening now; that is exactly why `wspike` exists.

The five fields, in order:

| field | meaning | the lever if it dominates |
|---|---|---|
| `rom` | ROM translation-cache flush | 256 KiB hash memset + up to 2 MiB of JIT code discarded and re-translated. Flush deliberately at a safe moment. |
| `full` | RAM JIT cache **ran out of room** | A bigger `RAM_TRANSLATION_CACHE_SIZE` (384 KiB on PSP vs 512 KiB elsewhere). |
| `smc` | a **CPU store** landed on RAM holding translated code | A bigger cache does **nothing**. Needs finer-grained invalidation — real surgery (ADR-0029). |
| `dma` | a **DMA** did the same | As above, but a DMA knows its destination range up front, so a range-invalidate is at least conceivable. |
| `page` | **ROM paging** — a 32 KiB read from the storage medium mid-emulation | Only possible when the ROM does not fit in RAM. Different fix entirely. |

All zero with a large `winmax` means **none of these**: the hunt moves inside the emulation loop
and the next step is bracketing `retro_run` by phase (audio / video / CPU).

A per-window *average* would hide a once-a-minute event, and an all-time snapshot goes stale.
The rig proved the second point: a window logging **4110 RAM flushes** still showed `spike=` all
zeros, because the all-time maximum had been set during boot.

**Why the split exists at all (ADR-0029).** Before it, `full` and `smc` were a single `ram` field
and the plan of record was to spend ~128 KiB of a ~372 KiB hardware memory budget enlarging the
JIT cache. The split killed that in one run: on the rig, during a full trade, both consoles log
**`win=0/0/4109/1/0`** and **`wspike=0/0/307/0/0`** — i.e. `full=0`, **cache exhaustion never
happens**, the cache is wiped by self-modifying code long before it can fill, and it is CPU
stores rather than DMA. Boot, solo, 3600 frames: `win=1/0/70/0/0`.

**What the rig can and cannot price.** PPSSPP holds the whole ROM in RAM, so `page` is
structurally 0 there — the mechanism ADR-0026 priced at 34 ms per 4 KiB on the user's memory
stick does not exist in the harness. It *can* price the flushes by subtraction: the rig's worst
frame was 8456 µs against a 5325 µs window mean with 307 flushes in it, i.e. **~10.2 µs per
flush** on a fast x86 host. **On hardware the same line is expected to look different, and that
difference is the finding.**

`EVT sess_cost` carries the same data on its 10 s session cadence as
`corewin=…/…/…/…/…  corespike=…/…/…/…/…`.

#### `EVT audio_rate` — the resampler's input rate is no longer assumed (ADR-0028)

```
EVT av_info fps=59.7275 rate=32768 w=240 h=160
EVT audio_rate in=32768 out=44100 step=48695
```

`gpsp_sound_rate` moved 65536 → **32768** (real GBA PWM bandwidth; the core's own option text
says it "halves audio mixing work" while keeping timing exact). The PSP frontend's `in_rate` used
to be a hardcoded `65536`, and `audio_start()` runs *before* `fe_host_boot()` — so the rate is now
read back from the core with `fe_host_sample_rate()` after boot. **If `av_info rate=` and
`audio_rate in=` ever disagree, the resampler is desynced from the core and audio will play at
the wrong speed into a draining ring.** That pair of lines is the check.

#### `--slow-join=US` and `--pace-match=0|1` — peer frame-rate matching (added 2026-08-02, ADR-0027)

**Read this before trusting a green pace-matching run.** The rig launches two PPSSPP instances
that both hold ~59.7 fps, so it **does not reproduce the field's role asymmetry** (PSP-3000:
58 fps as host, 46 as join; PSP-1000: 56-58 vs 49-52). ADR-0027 only engages when the two
peers' capabilities actually differ, so in a default run **it correctly does nothing** — and
that inertness is itself asserted, because pace matching that fired on a healthy symmetric
session would cost speed for no reason:

```
pace_match: inert on symmetric peers, as intended
```

`--slow-join=US` manufactures the asymmetry: the **join** instance burns `US` microseconds of
**busy** work per frame (autopilot key `pace_slow_us`, join only). Busy rather than sleeping is
deliberate — it must land in the same per-frame *work* time the capability estimate reads, or
the test would exercise the wrong path.

**`--slow-join=6000` is the recommended value: it walks the join instance from ~59 fps down
past the 40 fps floor over the course of a run, so a single execution exercises engage, ramp,
floor and clean release.** Observed host ladder (artifact
`psptrader40-20260802-003156/host.log`):

```
EVT pace_match target=57.73 self_cap=59.71 peer_cap=55.37 engaged=1 why=engage
EVT pace_match target=55.87 self_cap=59.71 peer_cap=54.71 engaged=1 why=ramp
...                                                        (2.00 fps per window)
EVT pace_match target=48.26 self_cap=59.71 peer_cap=43.42 engaged=1 why=ramp
EVT pace_floor peer_cap=39.93 floor=40.00 — not chasing
EVT pace_match target=50.26 self_cap=59.71 peer_cap=39.93 engaged=0 why=release
```

**`self_cap` is 59.71 on every line** while `target` walks 57.73 → 48.26. That is the
anti-ratchet property, observed rather than argued. The join side logged `engaged=1` **zero**
times, and saw `peer_cap=59.71` throughout — i.e. the host's advertised capability did not move
while the host was actively throttling itself, which is the same property seen from the
receiving end. The trade still completed and both mons swapped.

With `--slow-join` set, the run asserts the properties that matter:

| assertion | why it exists |
|---|---|
| host logs `pace_match … engaged=1` | the fast side must actually pace down |
| join logs **no** `engaged=1` | **exactly one mover** — both sides throttling is the bug |
| host `self_cap` does not sag while engaged | **the anti-ratchet guard** (see below) |
| no `target=` below 40.00 | the floor holds |

**The ratchet, and why `self_cap` is the field's tell.** If a console advertised its *achieved*
fps, the pair would spiral: A throttles to 46 to match B, A now reports 46, B throttles to
match, A re-measures lower, both crawl to the floor — presenting as a performance regression
rather than a control-loop bug. ADR-0027 advertises **capability** instead, computed from
per-frame work time with every vblank wait excluded, so a console's own throttle cannot move
its own advertised number. **`self_cap` staying flat while `engaged=1` is the proof that
holds**, in the harness and in the field log alike.

**EVERYTHING ABOVE IS NOW MODE 2 (`--pace-match=2`).** ADR-0033 remapped the key and made
fixed-rate clamping the default — see the next section. The adaptive assertions above only run
when the log reports `mode=2`; the script reads the mode out of `EVT net_pace_match` rather
than assuming it, so an A/B run gates itself correctly either way.

`--pace-match=0|1|2` pins `net_pace_match` for an A/B without touching `config.ini`; the same
lever exists on hardware (`config.ini net_pace_match`, default 1 = fixed-rate) so the user can
A/B on the consoles with no rebuild, exactly like `net_tx_thread` / `log_thread` /
`sram_thread`.

**What the rig cannot settle:** whether equal frame rates actually make the trade complete.
That is a hardware question — the rig proves the control loop is correct, converges, and
does not ratchet; only the consoles can prove it fixes the game.

#### Fixed-rate session pacing — the default (added 2026-08-02, ADR-0033)

`net_pace_match=1` no longer runs a control loop at all: while a session is live both consoles
clamp to the **same constant**, `net_session_fps` (**default `29.97` since ADR-0035 — was
40.00**), glide into it at 4.00 fps/s and glide back out after teardown. `--session-fps=F` pins
it per run.

**The requested rate is SNAPPED to `59.94/N` (ADR-0035).** The field achieved 35-38 fps against
a 40.00 clamp because whole-vblank pacing cannot average 40 unless frames fit inside one vblank,
and they do not. Achievable rates are 59.94, 29.97, 19.98 — nothing in between. A snap that
moves the number logs `EVT session_pace_snap req=… applied=… vblanks=…`, and
`EVT net_pace_match` carries `fixed=` (applied) beside `req=`. `net_session_fps_snap=0` disables
snapping so the raw request can be A/B'd against the snapped one.

**Unlike the adaptive matcher, this is NOT inert on symmetric peers**, which is what makes the
rig able to exercise it fully despite running both instances at full speed. The default
`run_trade_test_psp.sh` run asserts:

| assertion | why it exists |
|---|---|
| both sides log `session_pace … reason=session_start` | the policy came up on both |
| both `fps=` values are **byte-identical** | the entire determinism claim — no negotiation, so a mismatch means one side read a different config |
| both log `reason=ramp_done` | the glide actually reached the clamp; announcing it is not the same as applying it |
| both log `reason=session_end` | the clamp is released, nothing latches |
| **no** `EVT pace_match` line on either side | the mode-2 control loop must not run in mode 1 |
| `EVT session_pace_miss` → **warning, not failure** | by design: a console that cannot hold the rate is reported and not chased (ADR-0033). On the rig it would mean the rig itself cannot hold 40 fps, which is worth knowing but is not this feature's bug |

Observed on the rig (`--radio=40`, join instance, artifacts
`tools/e2e/artifacts/psptrader40-20260802-174850/`):

```
EVT net_pace_match mode=1 policy=fixed nominal=59.73 floor=40.00 fixed=40.00 ramp=4.00
EVT session_pace fps=40.00 reason=session_start
EVT session_pace fps=40.00 reason=ramp_done                        (~5 s later)
EVT session_pace_miss actual=38.28 fixed=40.00 self_cap=58.81 — not chasing
EVT session_pace fps=59.73 reason=session_end
EVT sess_cost … pace=40.00/59.11/58.83/1
```

**That `session_pace_miss` line is the sanity rule proving itself**, and it was not staged: the
rig was under heavy contention and briefly delivered 38.28 fps against a 40.00 clamp. The
policy logged it and **did not move the target** — the very next `sess_cost` still reads
`pace=40.00/…`, and `self_cap=58.81` shows the console was capable throughout. That is exactly
the required behaviour: report, do not chase. `pace=40.00/59.11/58.83/1` also shows the ADR-0027
capability exchange still working underneath (both `self_cap` and `peer_cap` near nominal while
the applied target sits at 40.00), which is what makes the clamp visibly a *choice* rather than
a limit.

**What the rig still cannot settle:** the field's host/join asymmetry does not exist here (both
PPSSPP instances run at full speed), so the rig cannot show the clamp *rescuing* a session. It
shows the clamp is applied, identical, and reversible. Only the consoles can show it works.

Both runs also now report `core=avg/max` (the cost of `retro_run` itself, ADR-0027
§measurement — ADR-0021 priced everything *except* the core) and
`pace=target/self_cap/peer_cap/engaged`, e.g.:

```
host core/pace: core=6742/10500 pace=59.73/59.12/59.71/0
join core/pace: core=5829/10501 pace=59.73/59.12/59.71/0
```

`peer_cap=59.71` on both sides is the fps exchange working end to end over the wire; `engaged=0`
is the policy correctly declining to act on peers that already agree.

#### `--roms=HOST,JOIN` — cross-edition legs (added 2026-08-01, Phase 4)

Editions: `emerald`, `firered`, `leafgreen`. Default `emerald,emerald`, which is byte-for-byte
the Gate-4 leg described above. Everything that varies per edition — ROM path, parked-save
fixture, trade input script, the Union Room map id asserted at the end, and the `.sav` party
layout the oracle decodes — lives in one table at the top of `run_trade_test_psp.sh`, so a new
leg is data, not code. Artifacts land in `artifacts/psptrade[r40]-<hh><jj>-<ts>/`.

Before anything launches, the script now verifies each ROM's **header game code (0xAC) and
revision byte (0xBC)** against the edition it was asked for — Emerald BPEE rev 0, FireRed BPRE
**rev 1**, LeafGreen BPGE **rev 1** (ADR-0009). The autopilot address tables are per-revision
constants, so a swapped image would otherwise surface as a baffling predicate timeout hundreds
of frames in rather than as "wrong ROM".

**Parked saves for FR/LG are supplied, not generated** (ADR-0022). Drop them in as
`testdata/fixtures/firered_parked.sav` / `leafgreen_parked.sav` (or `_parkedA`/`_parkedB` for a
same-edition pair) and the leg runs; with none present the script fails immediately with the
path it wants. The contract those saves must satisfy — parked on a Pokémon Center 2F at
map-local (6,4) facing north at the Union Room attendant, Pokédex obtained, two or more non-egg
Kanto-dex party mons — is documented at the top of `testdata/fixtures/frlg_trade_host.inputs`
and derived in docs/AUTOPILOT.md.

**Cross-edition matrix status**

| leg | scripts | fixtures | status |
|---|---|---|---|
| emerald ↔ emerald | `emerald_trade_{host,join}.inputs` | generated (`make_trade_fixtures.sh`) | **PASSING** (Gate 4-E; also `--radio=40/80`, `--disconnect`) |
| firered ↔ leafgreen | `frlg_trade_{host,join}.inputs` | **awaiting user-supplied saves** | **BLOCKED on fixtures** — harness wired, scripts written, never run |
| firered ↔ emerald | as above, mixed | awaiting FR save | **BLOCKED on fixtures** |
| leafgreen ↔ emerald | as above, mixed | awaiting LG save | **BLOCKED on fixtures** |
| firered ↔ firered / leafgreen ↔ leafgreen | as above | needs `_parkedA` **and** `_parkedB` | **BLOCKED on fixtures** |

`--disconnect` is Emerald-only: the survival script (`emerald_trade_host_disc.inputs`) has no
FR/LG twin yet, and the harness says so rather than silently substituting the normal script.

Cross-edition trading is legal in both directions for Kanto-dex species without a National Dex;
FR/LG additionally refuses to trade your last mon and refuses eggs pre-National-Dex. See
docs/AUTOPILOT.md "Trade legality in FR/LG" for the exact rules and for why the FR/LG host
registers requesting **TYPE_NORMAL**.

The union-room/link/trade addresses the FR/LG scripts use are **symbol-derived but not yet
observed at runtime** (the "S" rows of the FR/LG table in docs/AUTOPILOT.md) for exactly the
same reason the legs are blocked. The first successful FR↔LG run is what promotes them; each
sits behind its own labelled `evt` mark so a wrong one fails as a named timeout, not as silent
misbehaviour.

---

## Phase-2 UI/video harness layer (2026-08-01)

Three new drivers (same sandbox recipe; run sequentially like the others).
Two rig-level facts they depend on:

- **GE readback needs softgpu.** Under the Vulkan/llvmpipe backend PPSSPP
  renders into host-GPU framebuffers and CPU reads of emulated VRAM return
  stale bytes. `--appendconfig` with `[Graphics] SoftwareRenderer = True`
  makes softgpu render into emulated VRAM, so the frontend's GE drawbuffer
  dumps (`gedump_at`, `testpat`, ui_demo screenshots) contain the real
  rendered bytes. (`--graphics=software` on the CLI is NOT equivalent — it
  forces a GL presentation window that dies under Xvfb.)
- **PPSSPP's ms0: dirents are 8.3-uppercased** (`emerald.gba` lists as
  `EMERALD.GBA`), so files the app derives from dirent names (saves,
  `.st0`) come back uppercase on the host side — glob, don't compare
  exact case.

| Script | What it proves | Key assertions |
|---|---|---|
| `run_gu_color_test.sh` | GU blit channel order on the real GE (hw finding a: RGB565 uploaded as GU_PSM_5650 renders R/B-swapped on hardware; PPSSPP's window hides it) | Phase A `testpat=1`: 8 known RGB565 bars through the production blit path, GE drawbuffer decoded with the REAL 5650 layout (R bits 0-4), exact match + black borders. Phase B: in-game GE dump region pixel-identical to the same frame's core-buffer BMP. Proven sensitive: pre-fix code fails with the exact hw symptom |
| `run_ui_smoke.sh` | UI v1 + FF feature (plan §8 / §4.4) | Phase A `ui_demo=1`: EVT ladder ui_open → state_save/state_load (.st0 round-trip) → settings (video_mode fit/stretch/1x + config_saved) → wireless panel → ui_demo_done/ui_close → clean exit; menu GE screenshots (softgpu); config.ini persisted. Phase B `simff=300`: ff_user on (mult_x10=20, frameskip engaged) → ff_user off → clean run |
| `run_silent_trade.sh` | ADR-0013 silent-wireless policy end-to-end | inst1 `silent=1` role-auto: silent_activate role=join → **silent_host promoted=1**; inst2 `silent=1` role=join: **silent_joined**; sav_backup both sides; then the full Gate-4E trade oracle (session/peer EVTs, RAM + .sav personality swap, net_stats overflow=0/acked>0, clean exits) over the silently-negotiated session |

#### `run_gu_color_test.sh --blit-mode=0|1|2` — the ADR-0034 staging-placement A/B

The same pixel-exact check is the **correctness** gate for the three blit staging placements
(`config.ini blit_mode`: 0 cached RAM + writeback, 1 uncached mirror, 2 VRAM). softgpu renders
into emulated VRAM, so the readback proves byte-identical output whichever mapping the CPU
wrote through. **All three legs pass**, and mode 2 logs `EVT blit_mode req=2 mode=2 name=vram`,
i.e. the VRAM bump allocation succeeded rather than silently falling back.

**HARDWARE HAS SINCE DECIDED THIS: `blit_mode` defaults to 2 (VRAM).** PSP-1000, µs/frame:
cached `stage 1337 / gu 1176 / tot 2513`, uncached `1041 / 1174 / 2215`, **VRAM `1071 / 728 /
1800`**. The win is mostly `gu` — the GE reading its texture from VRAM — which is the opposite of
what was predicted here. The three legs below remain the **correctness** gate for all three modes.

**The rig cannot settle COST, and it says so out loud.** `EVT blit_prof` from the three runs:

```
EVT blit_prof mode=0 name=cached   n=600 stage=699/829 gu=22/23 tot=721
EVT blit_prof mode=1 name=uncached n=600 stage=699/829 gu=22/23 tot=721
EVT blit_prof mode=2 name=vram     n=600 stage=699/829 gu=22/23 tot=721
```

Identical to the microsecond, maxima included — PPSSPP models neither Allegrex's caches nor
uncached write cost, so deleting an 82 KiB per-frame writeback changes nothing here. **Never
pick a `blit_mode` default from a rig number.** What the rig *does* establish, and usefully:
`stage` (the CPU conversion) is **97 %** of the blit and `gu` (the GE list plus the blocking
`sceGuSync`) is **3 %**, and `tot=721` matches ADR-0032's independently-written `blt` bracket of
722 µs — two separate brackets agreeing, which is what makes the split trustworthy.

Autopilot.ini keys added for this layer: `testpat`, `gedump_at`, `ui_demo`,
`simff`, `silent` (+ optional `role`), `blit_mode` — documented in psp/main_psp.c's
header. The generic-build XMB branding (title "PSP AGB", ICON0/PIC1 from
psp/assets/) is asserted by unpack-pbp (PARAM.SFO TITLE + byte-identical
images); PPSSPP game-list tile screenshot in
tools/e2e/artifacts/xmb_evidence.png.

---

## `run_exit_room_test.sh` — the exit-Union-Room case (written 2026-08-01, **NOT YET RUN**)

Drives the field report from the first successful trade session: after the trade completed, the
user walked out of the Union Room and the game showed its **fatal** wireless screen ("…please
turn off the power"). Our app did not crash — both field logs end with an orderly `net_down` +
`exit code=0` — so this harness asks the only question that matters: what does the emulated RFU
do when the **game** ends its wireless session while our netdrv session is still up and still
delivering the peer's in-room traffic?

- Scripts: `emerald_exit_room_host.inputs`, `emerald_exit_room_join.inputs`, and
  `emerald_exit_room_host_stay.inputs`. Each is the corresponding trade script plus a tail that
  walks the inbound path in reverse (raw coords = map + 7 MAP_OFFSET: column 3 or 2 down to
  row 10, right to column 7, down onto the entrance warp) and then **re-enters** the room.
- Ordering is pinned, not raced: the host leaves first, the joiner ~11 s later. Two avatars
  cannot share the entrance tile, and "who left first" is itself a variable.
- `--leaver=join` uses the `_stay` host script: the host idles hands-off in the room sampling
  `gMain.callback2` while the joiner walks out. This separates "a partner left" from "I left".
- **Oracle is direct.** `logram cb2_*` samples `gMain.callback2` at every interesting moment and
  `CB2_PrintErrorMessage|1 == 0x0800b1a1` **is** the fatal screen (docs/AUTOPILOT.md Gate-3
  table — the same predicate `run_trade_test_psp.sh --disconnect` uses). Indirectly, a script
  that lands on the fatal screen can never reach `CB2_Overworld` again, so it fails on its own.
- Also asserts our stack outlived the game's RFU session: `peers=1`, `overflow=0`, clean exits,
  and it prints the `EVT rfu_link_down` breadcrumbs (ADR-0019) plus per-side `EVT fps` and worst
  `EVT sram_flush ms=`.

**State: committed, never executed.** The navigation legs are derived from the (proven) inbound
paths but have not been run, so expect the first execution to need coordinate fixes. That is not
a defect in the harness — it is the untested part, and it is called out here so nobody reads a
first-run failure as a reproduction of the field bug.
