# Save States page round trips, GBA (docs/UI-OVERLAY.md section 13): the
# fixture card with every slot holding the real em.st0 (slot 5 a 3.1 state:
# .thumb, no .shot).  ss_rt.ui: save to an empty slot and load it back (the
# frames 60 after each must match), a 3.1 state and a .shot state of the
# same bytes load alike, overwrite the 3.1 slot (it gains a .shot), delete
# one; shot_probe prices the read.  MODEL=0/1.
N=1
D=$(dirname "$SPEC")
FX=${FX:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/savestate-ui-out/fx}
I1_EB=${EB:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/savestate-ui-out/s1-h64}
I1_ROM=$HOME/gbamulti/roms/em.gba
I1_ROMNAME="Pokemon - Emerald Version.gba"
I1_STATE=$HOME/gbamulti/fx/em.st0
I1_SAVE=$HOME/gbamulti/fx/em-host.sav
I1_THEME=0
I1_CONSOLE=0
I1_MODEL=${MODEL:-1}
I1_UI=$D/ss_rt.ui
I1_STAGE=$D/stage.sh
I1_INI='shot_probe = 1'
export SSFX=$FX/gba SSSUF='' SSSTATE=$HOME/gbamulti/fx/em.st0
I1_CFG='standby = 0'
TIMEOUT_S=400
