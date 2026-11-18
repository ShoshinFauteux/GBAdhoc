# Single-player frame identity (docs/GBA-LINK-UI.md): the AW2 scene tour
# (the owner's .st0 at frame 30 + aw2_psp_script.txt), guest-state hashes
# every frame (shash) and the audio oracle.  This branch, link menus off (the harness default).
N=1
D=$(dirname "$SPEC")
I1_EB=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_PLAIN=1
I1_ROM=$HOME/drprof/fx/aw2.gba
I1_STATE=$HOME/drprof/fx/aw2.st0
I1_SAVE=$HOME/drprof/fx/aw2.sav
I1_SCRIPT=$HOME/drprof/fx/aw2_psp_script.txt
I1_INI='shash = 1\naudio_oracle = 1\nautoexit_frames = 6061'
I1_CFG='standby = 0'
I1_UI=$D/empty.ui
TIMEOUT_S=900
