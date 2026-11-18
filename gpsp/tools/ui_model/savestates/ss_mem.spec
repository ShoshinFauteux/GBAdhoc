# Save States page, memory + VRAM tenancy (docs/UI-OVERLAY.md section 13):
# a real Emerald state, Fit + sharp bilinear + ambient (art, else game), so
# the backdrop goes through sharp_draw every frame and the bars through the
# ambient texture while the page fills the .shot texture.  Heap census
# before the menu, on the menu, on the page, after it closes.  MODEL=0
# (PSP-1000, 32 MiB) or 1 (64 MiB).
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
I1_UI=$D/ss_mem.ui
I1_STAGE=$D/stage.sh
export SSFX=$FX/gba SSSUF='' SSSTATE=$HOME/gbamulti/fx/em.st0
I1_CFG='standby = 0\nscale_gba = 1\nfilter_gba = 2\nambient_gba = 2'
TIMEOUT_S=300
