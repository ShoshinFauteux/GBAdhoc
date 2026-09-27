/* Per-ROM savestate paths and compact browser-preview format. */
#ifndef PSP_STATE_SLOTS_H
#define PSP_STATE_SLOTS_H

#include <stdio.h>
#include <string.h>
#include "rom_paths.h"
#include "fe_console.h"

#define PSP_STATE_SLOT_COUNT 5
#define PSP_STATE_THUMB_WIDTH 64
#define PSP_STATE_THUMB_HEIGHT 42
#define PSP_STATE_THUMB_TEX_HEIGHT 64
#define PSP_STATE_THUMB_HEADER_SIZE 16
#define PSP_STATE_THUMB_MAGIC "STH1"
#define PSP_STATE_THUMB_PATH_CAP (PSP_FILE_PATH_CAP + 7)

/* The slot-1 suffix, which replaces the ROM's extension.  GBA keeps the
 * historic ".st0" (Game.st0).  GB/GBC states keep the ROM's own extension
 * in the name -- Game.gb.st0, Game.gbc.st0 -- exactly as their battery saves
 * do (Game.gb.sav), so a GB game and a GBA game with the same stem can
 * never load each other's states.  The console, not the file extension,
 * decides: it is what the core was booted as. */
static inline const char *psp_state_slot1_suffix(fe_console_t console)
{
   return console == FE_CONSOLE_GBC ? ".gbc.st0" :
          console == FE_CONSOLE_GB  ? ".gb.st0"  : ".st0";
}

/* Slot 1 is the historic .st0 file; slots 2-5 use .st1 through .st4. */
static inline int psp_state_path_for_slot(char *out, size_t out_sz,
                                          const char *slot1_path,
                                          unsigned slot)
{
   char suffix[8];
   int n;
   if (slot < 1 || slot > PSP_STATE_SLOT_COUNT)
      return -1;
   n = snprintf(suffix, sizeof(suffix), ".st%u", slot - 1);
   if (n < 0 || (size_t)n >= sizeof(suffix))
      return -1;
   return psp_rom_path_suffix(out, out_sz, slot1_path, suffix);
}

static inline int psp_state_thumb_path(char *out, size_t out_sz,
                                       const char *state_path)
{
   int n;
   if (!out || !out_sz || !state_path)
      return -1;
   n = snprintf(out, out_sz, "%s.thumb", state_path);
   if (n < 0 || (size_t)n >= out_sz)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

#endif
