# Save States page GE walk (docs/UI-OVERLAY.md section 13): gba, theme 1.
N=1
D=$(dirname "$SPEC")
FX=${FX:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/savestate-ui-out/fx}

I1_EB=${EB:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/savestate-ui-out/s1-h64}
I1_ROM=$HOME/gbamulti/roms/em.gba
I1_ROMNAME="Pokemon - Emerald Version.gba"
I1_THEME=1
I1_CONSOLE=0
I1_BACKDROP=$FX/gba/backdrop.565
I1_UI=$D/ge_gba.ui
I1_STAGE=$D/stage.sh
export SSFX=$FX/gba SSSUF=''
I1_CFG='standby = 0'
TIMEOUT_S=300
