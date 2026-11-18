# Mario Kart: Super Circuit Link Cable room: console 1 (the 3000's 64 MiB
# model, harness64) hosts by menu, console 2 (the PSP-1000's 32 MiB model,
# harness) joins through the scan list; both drive their karts from RAM
# (multigame fixture scripts, held until the link runs); the host ends by
# agreement 1500 frames after its script is done (the fixture's END_AFTER).
N=2
D=$(dirname "$SPEC")
EB64=${EB64:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h64}
EB32=${EB32:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/gbalink-ui-out/u1-h32}
MG=$D/../multigame/scripts
COMMON_INI='gba_dual_hash = 1\nlink_exit_after = 240\ngblink_end_after = 1500'

I1_EB=$EB64
I1_MODEL=1
I1_ROM=$HOME/gbamulti/roms/msc.gba
I1_ROMNAME="Mario Kart - Super Circuit (USA).gba"
I1_STATE=$HOME/gbamulti/fx/msc.st0
I1_THEME=${THEME:-1}
I1_INI=$COMMON_INI
I1_CFG='standby = 0'
I1_SCRIPT=$MG/msc_host.txt
I1_UI=$D/msc_host.ui

I2_EB=$EB32
I2_MODEL=0
I2_ROM=$HOME/gbamulti/roms/msc.gba
I2_ROMNAME="Mario Kart - Super Circuit (USA).gba"
I2_STATE=$HOME/gbamulti/fx/msc.st0
I2_THEME=${THEME:-1}
I2_INI=$COMMON_INI
I2_CFG='standby = 0'
I2_SCRIPT=$MG/msc_join.txt
I2_UI=$D/msc_join.ui

TIMEOUT_S=2400
