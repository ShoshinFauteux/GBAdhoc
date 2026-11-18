# Save States page GE walk (docs/UI-OVERLAY.md section 13): gbc, theme 0.
N=1
D=$(dirname "$SPEC")
FX=${FX:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/savestate-ui-out/fx}
OUTUI=/tmp/ss_ge_gbc_t0.ui; sed 's/SCENE/gbc/' $D/ge_gbx.ui > $OUTUI
I1_EB=${EB:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/savestate-ui-out/s1-h64}
I1_ROM=$HOME/gbdual/roms/crystal.gbc
I1_ROMNAME="Pokemon - Crystal Version.gbc"
I1_THEME=0
I1_CONSOLE=2
I1_BACKDROP=$FX/gbc/backdrop.565
I1_UI=$OUTUI
I1_STAGE=$D/stage.sh
export SSFX=$FX/gbc SSSUF='.gbc'
I1_CFG='standby = 0'
TIMEOUT_S=300
