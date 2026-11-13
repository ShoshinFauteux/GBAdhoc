# setup snippet for run_ppsspp.sh: the quick load's browser through the real
# menu, on a lab (or harness) EBOOT.  Emerald with a state in slots 1 and 3
# and none in 2, 4 and 5; the route is tools/ui_model/quickload_route.txt, a
# ui_script (docs/GBA-LINK-UI.md): it presses the quick load's chord in the
# running game, reads the question off the screen, browses and answers.
# in WSL: OUT=... bash tools/ui_model/run_ppsspp.sh quickload <eboot dir> tools/ui_model/setup_quickload.sh
# Pass: frontend.log ends `EVT exit code=0 reason=ui_script` and holds
# `EVT state_load file=...emerald.st2`.
ROM="emerald.gba"; STEM="emerald"
cp "$FX/emerald.gba" "$GAME/roms/$ROM"
cp "$FX/emerald_3000.sav" "$GAME/roms/$STEM.sav"
cp "$FX/emerald_3000.st0" "$GAME/roms/$STEM.st0"
cp "$FX/emerald_go.st0" "$GAME/roms/$STEM.st2"
cp "$HERE/quickload_route.txt" "$GAME/uis.txt"
{
  echo "rom = $ROM"
  echo "heartbeat_s = 5"
  echo "autoexit_frames = 30000"
  echo "ui_script = uis.txt"
  echo "me_mode = 1"
} > "$GAME/.gpsp-harness.ini"
true
