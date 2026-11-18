# Save States page round trip, GB or GBC (CON=gb|gbc): an empty card, save
# slot 1 from the running game, load it back; the frames 60 after each must
# match (ss_rt_gbx.ui).  MODEL=0/1.
N=1
D=$(dirname "$SPEC")
I1_EB=${EB:-${GBADHOC_WORK:?run through run_linkmenu_ppsspp.sh or set GBADHOC_WORK}/savestate-ui-out/s1-h64}
case "${CON:-gb}" in
  gbc) I1_ROM=$HOME/gbdual/roms/crystal.gbc; I1_ROMNAME="Pokemon - Crystal Version.gbc"; I1_CONSOLE=2 ;;
  *)   I1_ROM=$HOME/gbdual/roms/kirby.gb;    I1_ROMNAME="Kirby's Dream Land.gb";        I1_CONSOLE=1 ;;
esac
I1_THEME=0
I1_MODEL=${MODEL:-1}
I1_UI=$D/ss_rt_gbx.ui
I1_INI='shot_probe = 1'
I1_CFG='standby = 0'
TIMEOUT_S=300
