# RetroArch-PC ×2 reference rig — Gate 0's last item

Purpose (plan §3.2, §5 Phase 0): experience the **known-good** RFU setup — two RetroArch
instances + stock gpsp core completing a Union Room trade — before we build anything, and keep
the rig around as the "is it us or upstream?" bisection tool for the whole project.

**The rig is fully staged at `C:\gpsp-adhoc-tools\ra-rig\`** (local disk, off OneDrive):
RetroArch 1.22.2 win64 (netpacket API ≥1.17 ✔), gpsp core nightly dll, real BIOS in `system/`,
Emerald ROM in `roms/`, and per-instance saves: **instA = your real save** (as `.srm`),
**instB = the CLONEB clone** (distinct trainer name/TID via `tools/e2e/clone_sav.py`, so the
Union Room sees two different trainers). Core options are pre-set per instance:
`gpsp_bios=official`, `gpsp_serial=auto` (auto-resolves Emerald→RFU), frameskip off.
`pause_nonactive=false` is pre-set so both windows keep running while unfocused.

## The 15-minute session (when you're home)

1. Double-click `run-instA.bat`, then `run-instB.bat`. Two Emerald windows boot with their saves.
2. **Instance A** (your save): press `F1` → Netplay → **Host** → *Start Netplay Host*.
3. **Instance B**: `F1` → Netplay → **Connect to Netplay Host** → address `127.0.0.1`, default
   port (55435). A toast on both sides confirms the connection.
4. In both games: Pokémon Center → **top floor** → speak to the middle attendant (Union Room) →
   enter. The two games should discover each other (you'll see the other trainer appear).
5. Greet → Trade: complete one trade (any junk Pokémon each way is fine — trade them back after
   if you like).
6. After the trade completes and both games have saved (in-game save prompt), quit both
   RetroArchs. Done.

**Record (tell me in chat, I'll log it in DECISIONS.md):**
- Did discovery/greeting/trade work first try? Anything janky (delays, retries, disconnects)?
- Rough time from Union Room entry to seeing the other player: ______
- RetroArch version shown in the main menu (should be 1.22.2) and that both saves survived
  (relaunch instA — your Pokémon post-trade state intact).

Default keyboard controls if needed: arrows = D-pad, `X`=A, `Z`=B, `Enter`=Start,
`RShift`=Select, `Q`/`W`=L/R, `F1`=menu, `Esc Esc`=quit.

## Troubleshooting

- Core fails to load / instant crash: tell me — I'll pin a different core nightly.
- Netplay connects but Union Room never sees the peer: first check instance B actually loaded
  the CLONEB save (trainer name on the trainer card). Then tell me — that would be an upstream
  reproduction question, exactly what this rig exists to answer.
- Your real save is never written by instance B; instA writes only to
  `ra-rig\saves\instA\*.srm` (a copy — the original + backup live in `testdata/`).

## Rig rebuild

Everything here is reproducible: RetroArch 1.22.2 from libretro buildbot stable, core dll from
`nightly/windows/x86_64/latest`, configs in `ra-rig\config\*.cfg` (committed knowledge lives in
this doc; the rig folder itself is disposable).
