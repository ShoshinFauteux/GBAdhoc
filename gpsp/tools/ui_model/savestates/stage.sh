# stage.sh -- sourced by run_linkmenu_ppsspp.sh (Ii_STAGE) for the Save
# States page (docs/UI-OVERLAY.md section 13): the fixture slots of
# make_fixtures.py onto the emulated card, with their mtimes (the page's
# timestamps).  env: SSFX (make_fixtures OUTDIR/<con>), SSSUF ("" for GBA,
# ".gb" / ".gbc": the state names keep the ROM's extension), SSSTATE (a real
# state to copy into every slot; default 4 KiB of zeros -- the page only
# stats it).
for k in 1 2 3 4 5 6 7 8 9 10; do
  [ -f "$SSFX/slot$k.time" ] || continue
  st="$ROMSTEM${SSSUF:-}.st$((k - 1))"
  if [ -n "${SSSTATE:-}" ]; then cp "$SSSTATE" "$st"; else head -c 4096 /dev/zero > "$st"; fi
  cp "$SSFX/slot$k.thumb" "$st.thumb"
  [ -f "$SSFX/slot$k.shot" ] && cp "$SSFX/slot$k.shot" "$st.shot"
  t=$(cat "$SSFX/slot$k.time")
  touch -d "$t" "$st" "$st.thumb"
  [ -f "$st.shot" ] && touch -d "$t" "$st.shot"
done
ls -l --time-style=+%F_%R "$GAME/roms" >> "$OUT/stage.txt"
