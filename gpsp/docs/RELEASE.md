# RELEASE.md — the runbook

Phase 6 / Gate 6: **a release zip a stranger can install from the README alone.**

This is a checklist, not a narrative. Work top to bottom. Nothing here builds itself;
`tools/make_release.sh` deliberately packages only, so the binary you ship is the binary you
tested.

---

## 1. Preconditions — all of these, no exceptions

Do not tag anything until every box is genuinely ticked. "Probably fine" is how a stranger
ends up with a zip that hard-locks their console.

| # | Precondition | How you know |
|---|---|---|
| P1 | **The real-radio wireless fix has landed on `main`** | The correctness fix (never silently drop a RELIABLE payload — backpressure or visible session failure) plus the adaptive RTO with a PSP-ad-hoc floor. See CHANGELOG "Known issues" and `docs/HANDOFF.md`. |
| P2 | **netdrv unit suite green** | `make -C netdrv test` (or `netdrv/test_netdrv`) exits 0, including the new assertion that no RELIABLE payload is ever dropped under sustained overload |
| P3 | **Desktop e2e green** | `tools/e2e/run_netsmoke.sh`, `run_trade_test.sh`, `run_trade_test.sh --disconnect` all exit 0 |
| P4 | **PSP e2e green** | `tools/e2e/run_boot_test.sh`, `run_save_test.sh`, `run_soak.sh`, `run_gu_color_test.sh`, `run_ui_smoke.sh`, `run_nettest.sh`, `run_trade_test_psp.sh`, `run_trade_test_psp.sh --disconnect`, `run_silent_trade.sh` — all exit 0, artifacts under `tools/e2e/artifacts/` |
| P5 | **Latency-injection run passes** | The adhoc transport's debug shim run at ≥ 40 ms RTT + jitter — the profile that reproduces the field failure. A trade that completes there is the evidence P1 actually worked, because localhost never could have caught it. |
| P6 | **Gate 4-H passed on real hardware** | `docs/HARDWARE-ACCEPTANCE.md` run once on two real PSPs, both `frontend.log`s received and reviewed: `overflow=0`, no `arq overflow` line, `retx/acked` well under 0.5, `core_tx`/`core_rx` climbing throughout the trade, saves intact after every step |
| P7 | **CHANGELOG is current and honest** | The `[0.1.0]` heading has a date, and "Known issues" reflects reality *after* the fix — delete what's fixed, keep what isn't |
| P8 | **README claims match the code** | Every control, path and menu item still exists. If the UI moved, the README moves. |
| P9 | **Working tree clean, on `main`** | `git status` empty; the release stamp embeds the commit and marks `-dirty` otherwise |

If P6 cannot happen before you want to ship, **ship nothing** — or ship explicitly as a
pre-release, with the README status section saying so in the same words the CHANGELOG uses.

---

## 2. Build

Exactly these commands, from a clean checkout of the tagged commit. Docker supplies the
toolchain so the build is reproducible on any machine.

```bash
# 0. clean slate — stale objects from a desktop build must never reach the PSP link
git clean -xdf -e testdata -e tools/e2e/artifacts     # check what this will delete first!

# 1. the gpSP core, PSP target (MIPS32 Allegrex, dynarec on)
docker run --rm -v "$PWD":/build -w /build pspdev/pspdev \
    sh -c 'make platform=psp1 clean && make platform=psp1'
#    -> gpsp_libretro_psp1.a

# 2. the frontend + EBOOT
docker run --rm -v "$PWD":/build -w /build/psp pspdev/pspdev make
#    -> psp/EBOOT.PBP   (XMB title "PSP AGB", ICON0/PIC1 from psp/assets/)
```

Sanity-check the artefact before packaging it:

```bash
ls -l psp/EBOOT.PBP          # ~1.5 MiB; a few-hundred-KiB EBOOT means the core didn't link
```

Then boot it once in PPSSPP (`tools/e2e/run_boot_test.sh`) — the last chance to catch a
broken link before it reaches a memory stick.

---

## 3. Package

```bash
tools/make_release.sh psp/EBOOT.PBP --version 0.1.0
```

The script will:

- refuse if the EBOOT is missing, implausibly small, or **older than any source in
  `psp/`, `frontend-common/`, `netdrv/`** or the core archive (`--allow-stale` overrides —
  never use it for a real release);
- lay out `PSP/GAME/gpsp-adhoc/` with the EBOOT and empty `roms/`, `saves/`, `log/`;
- write `README.txt` (install text with the WLAN-switch and ad-hoc-channel warnings first),
  `LICENSE` (copied from `COPYING`), and a `VERSION.txt` stamp with version + commit + date;
- **refuse to package any ROM, BIOS image or save file** — by extension and by GBA header
  magic;
- zip to `dist/psp-agb-<version>.zip` and print a manifest with every file's size and
  sha256, plus the zip's own.

**Keep the manifest.** Paste the zip's sha256 into the release notes.

### Verify the package like a stranger would

1. Extract the zip somewhere fresh.
2. Copy the `PSP` folder to a memory stick that has **never** had this app on it.
3. Add one ROM to `roms/`. Add nothing else.
4. Boot it on a real PSP, following **only** `README.txt`. If you have to know something the
   file doesn't say, the file is wrong — fix it and re-package.
5. Confirm `log/frontend.log` appears and `EVT exit code=0` is the last line after a clean
   exit.

---

## 4. Tag

```bash
git tag -a v0.1.0 -m "PSP AGB v0.1.0 — GBA wireless multiplayer on PSP ad-hoc"
git show v0.1.0 --stat | head -20
# push once a remote exists (ADR-0002: repo hosting is still a user decision)
# git push origin main --follow-tags
```

`make_release.sh` picks the version up from the latest tag when `--version` is omitted, so
tagging *before* packaging also works — just don't let the tag and the zip name disagree.

---

## 5. Publish

Attach to the release:

| Artefact | Notes |
|---|---|
| `psp-agb-<version>.zip` | the only thing an end user needs |
| The sha256 of that zip | from the manifest |
| Release notes | the `[0.1.0]` CHANGELOG section, verbatim — including **Known issues** |

Release notes must open with credit (davidgfnet for the Wireless Adapter
reverse-engineering and emulation; afska and Corwin for the protocol documentation;
libretro/gpsp upstream) and must state GPL-2.0. Source availability is a licence obligation,
not a nicety: the tag must be reachable wherever the binary is.

**Do not attach**: ROMs, BIOS dumps, save files, or a "test ROM bundle". The packaging
script refuses to build them into the zip; don't undo that by hand.

---

## 6. Upstream courtesy — do this, it matters

We are shipping a product built on someone else's hardest work. Two things go back.

### 6a. The `rfu.c` queue fix (ADR-0011)

Offer as a PR to **davidgfnet** (and/or libretro/gpsp): `RFU_PKT_QUEUE 4 → 16`.

Frame it for *his* users, not ours: the core buffers netpacket-delivered link frames in
fixed 4-deep per-direction queues and **silently discards** overflow. Depth 4 assumes an
essentially zero-latency transport. Any transport with real latency — ours, or **RetroArch
netplay over a WAN** — delivers legitimate clumps deeper than 4; gen-3 Pokémon never
retransmit their one-shot command packets; so a dropped clump wedges both games with zero
diagnostics. Reproducible deterministically at 10–30 ms injected jitter, and *impossible* to
hit on a sub-millisecond localhost rig, which is why it hasn't surfaced upstream.

Include in the PR:
- the diff (two array sizes, two enqueue loops, two dequeue memmoves — no logic change,
  behaviour-identical at zero latency);
- the observed failure: "Host dropped a client packet" ×407 in one run, both games wedged
  in `WAIT_FOR_RESPONSE` / `HANDLE_CONTACT`;
- the arithmetic: ~250 ms of clumped deliveries absorbed at the link's steady ~2
  frames-per-frame rate;
- confirmation it compiles clean for `unix` and `psp1`.

Also offer the **weak `gpsp_rfu_activated_hook()`** (ADR-0013) in the same conversation or a
follow-up: 8 lines, a no-op for every existing frontend, and generically useful to any
frontend that wants to know the moment a game starts using the adapter.

And pass along the quirk list in `docs/SERIAL-PROTO-NOTES.md` — the core-behaviour details
found while reading `rfu.c` / `serial_proto.c` that aren't bugs but aren't written down
anywhere either.

### 6b. The netdrv / transport split RFC (plan §5 Phase 6)

Open an **issue/RFC**, not a PR, on davidgfnet's repo. His own TODO list asks for exactly
this: *"Bringing back the native UI for PC, PSP and perhaps PS2/3DS/Wii"* and *"A native UI
with Multiplayer support for portable devices with wifi support."* We built one. The RFC
should propose the split we already live with:

- a **transport-agnostic reliable-datagram layer** implementing the `retro_netpacket`
  contract (ARQ, roster, framing, liveness) that is entirely independent of *how* bytes
  move;
- **pluggable transports** behind a small vtable — UDP for desktop/dev, `sceNetAdhoc` PDP
  for PSP, and whatever a future platform needs;
- what we learned that a shared implementation should bake in: the core sends exclusively
  `RELIABLE|FLUSH_HINT` so the unreliable path is cold; the RFU's real maximum payload is
  104 bytes (frame budgets sized for cable modes waste queue depth); RTO must be adaptive,
  because a floor tuned on loopback is a death spiral on real radio; and **a reliable
  transport must never silently drop a payload** — the RFU state machine cannot recover.

Offer the code. The worst case is a polite no and the project keeps its own driver; the best
case is that PSP, and the next portable after it, stop being a fork.

---

## 7. After

- Move any deferred item out of "Known issues" into an issue tracker or `docs/HANDOFF.md`
  so it doesn't rot inside a changelog.
- Record the Gate 4-H result (numbers, not adjectives) in `docs/DECISIONS.md` as a gate
  record, with the log artefacts kept.
- If the user has hosting preferences to settle (ADR-0002 is still open), settle them before
  the first public link goes out — a release with no reachable source is a licence problem.
