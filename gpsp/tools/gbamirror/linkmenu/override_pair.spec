# Per-game override (roms/linkmode.txt): two independent consoles hosting.
N=2
START2=
D=$(dirname "$SPEC")
EB64=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
I1_EB=$EB64
I1_ROM=$HOME/gbamulti/roms/em.gba
I1_ROMNAME="Pokemon - Emerald Version.gba"
I1_STATE=$HOME/gbamulti/fx/em.st0
I1_SAVE=$HOME/gbamulti/fx/em-host.sav
I1_CFG='standby = 0'
I1_LINKMODE='# test\nPokemon - Emerald Version.gba = cable\n'
I1_UI=$D/override_host.ui
I2_EB=$EB64
I2_ROM=$HOME/gbamulti/roms/aw2.gba
I2_ROMNAME="Advance Wars 2 - Black Hole Rising.gba"
I2_STATE=$HOME/gbamulti/fx/aw2.st0
I2_SAVE=$HOME/gbamulti/fx/aw2.sav
I2_CFG='standby = 0\ngroup = GPSP08'
I2_LINKMODE='Advance Wars 2 - Black Hole Rising.gba = adapter\n'
I2_UI=$D/override_host.ui
TIMEOUT_S=300
