# tools/

Build, test, rig and analysis tools.  The build is `tools/build.sh <profile>`; the host tests are
`python tools/run_host_tests.py`.

## Local folders

Many rig and test scripts read inputs that earlier runs produced (EBOOT directories, PPSSPP
fixtures, rig stages) or a ROM library.  None of that is in git.  The scripts find them through
these environment variables; `tools/workdirs.sh` (shell) and `tools/workdirs.py` (Python) compute
the defaults.  Set a variable to override its default.

| variable | what | default |
|---|---|---|
| `GBADHOC_REPO` | the repository root | the checkout the script is in |
| `GBADHOC_WORK` | local build outputs, rig stages, fixtures made by earlier runs | the first `builds/` folder at or above `GBADHOC_REPO` |
| `GBADHOC_ROMS` | the ROM library (a list, `:`-separated, for `drprof/dr_corpus.py stage`) | `GBADHOC_REPO/roms` |
| `GBADHOC_BIOS` | a GBA BIOS for `drprof/dr_corpus.py` when `~/drprof/fx/gba_bios.bin` is absent | none |
| `GBADHOC_RIG_ASSETS` | the rig assets folder `stage_cards.ps1` copies saves from (or `-Assets`) | none: required |

Scripts that used to name a folder under the owner's `builds/` now default to the same folder under
`GBADHOC_WORK`, and keep their own override where they had one:

| script | variable (default) |
|---|---|
| `display-rig/*.sh` | `FIX` (`$GBADHOC_WORK/dynarec-profile-out/rig-stage-hnsdiag/base`) |
| `e2e/run_me_timing_sim.sh`, `e2e/run_me_midframe_fixtures.sh` | `MTS_FIX`, `MTS_ROMS` (`$GBADHOC_WORK/unbound-soak/fixtures`, `.../unbound-harness/fixtures`) |
| `gbamirror/linkmenu/*.spec`, `ui_model/savestates/*.spec` | `EB64`, `EB32`, `EB`, `FX`, `OLD`, `BASE`, `REL`, `BACKDROP` (under `$GBADHOC_WORK/gbalink-ui-out`, `gbalink-hw-out`, `savestate-ui-out`); `run_linkmenu_ppsspp.sh` sources `workdirs.sh` first |
| `gbamirror/mirhand_dry.sh` | `DRPROF_BASE` (`$GBADHOC_WORK/dynarec-profile-out/rig-stage/base`) |
| `gbamirror/sync_wsl.sh` | `SRC` (this gpsp tree) |
| `inproc/hw/run_soak*.sh` | `INPROC_SOAK_TREE` (this gpsp tree), `INPROC_SOAK_OUT` (`$GBADHOC_WORK/inproc-soak-out`) |
| `rewind_all.sh`, `rewind_campaign.sh`, `rewind_twin.sh` | `RW_SRC` (this gpsp tree); `rewind_all.sh` `RW_LIB` (`$GBADHOC_ROMS`) |
| `rewind_ppsspp.sh`, `rewind_ppsspp_save.sh` | `RW_OUTW` (`$GBADHOC_WORK/rewind-out/psp`), `RW_TOOLS` (this tree's `tools/`) |
| `rewind_savesweep.sh`, `rewind_survey.sh` | `RW_SAVS` (`$GBADHOC_ROMS`) |
| `share/run_share_ppsspp.sh` | `EB_A`/`EB_B` (arguments 3, 4), `FX`, `HERO` (`$GBADHOC_WORK/...`, `$GBADHOC_REPO/hero`) |
| `rig/mk_emerald_fixture.py` | `FIXTURE_DIR` (this tree's `testdata/fixtures`) |
| `push_scripts.ps1`, `stage_cards.ps1` | `-Repo` (this gpsp tree) |

From the main checkout the defaults are the folders these scripts always used.  From a worktree that
is not inside that checkout's `builds/`, set `GBADHOC_WORK` (and `GBADHOC_ROMS`) explicitly.
