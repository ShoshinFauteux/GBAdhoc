/* Per-ROM savestate paths and compact browser-preview format. */
#ifndef PSP_STATE_SLOTS_H
#define PSP_STATE_SLOTS_H

#include <stdio.h>
#include <string.h>
#include "rom_paths.h"
#include "fe_console.h"

/* TEN SLOTS A GAME, .st0 TO .st9.  4.5.0 and earlier had five (.st0-.st4);
 * those names keep their slots, and the five after them continue the same
 * rule.  Ten is also where the rule stops: the slot is ONE digit, which is
 * what lets psp_state_delete_paths (below) name a slot's files by replacing
 * the last character of the slot-1 path and nothing else.  Share game data
 * (fe_share.h FE_SHARE_SLOTS) still offers and receives slots 1-5 only: its
 * manifest is a wire format, and both ends must agree on it. */
#define PSP_STATE_SLOT_COUNT 10
/* The lists (the in-game SAVE STATES page, the browser's state shelf) were
 * drawn for five rows and keep them: with ten slots they scroll. */
#define PSP_STATE_SLOT_ROWS 5
#define PSP_STATE_THUMB_WIDTH 64
#define PSP_STATE_THUMB_HEIGHT 42
#define PSP_STATE_THUMB_TEX_HEIGHT 64
#define PSP_STATE_THUMB_HEADER_SIZE 16
#define PSP_STATE_THUMB_MAGIC "STH1"
#define PSP_STATE_THUMB_PATH_CAP (PSP_FILE_PATH_CAP + 7)

/* THE FULL-SIZE PREVIEW (docs/UI-OVERLAY.md section 13): `<state>.shot`, the
 * frame the player saw at native size, for the SAVE STATES page's panel.
 * The .thumb's 16-byte header ("STH1", w, h, stride as u16, 6 reserved), w x h
 * = 240x160 (GBA) or 160x144 (GB/GBC), then h rows stored at the TEXTURE's
 * stride of 256 texels, so the whole body is one read into a 256-wide
 * texture.  Like the .thumb it is a companion file: written after the state,
 * removed on a failed write, deleted with the state, and its absence (a 3.1
 * state) only ever costs the big picture. */
#define PSP_STATE_SHOT_MAX_W   240
#define PSP_STATE_SHOT_MAX_H   160
#define PSP_STATE_SHOT_STRIDE  256
#define PSP_STATE_SHOT_PATH_CAP (PSP_FILE_PATH_CAP + 6)

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

/* Slot 1 is the historic .st0 file; slots 2-10 use .st1 through .st9. */
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

static inline int psp_state_shot_path(char *out, size_t out_sz,
                                      const char *state_path)
{
   int n;
   if (!out || !out_sz || !state_path)
      return -1;
   n = snprintf(out, out_sz, "%s.shot", state_path);
   if (n < 0 || (size_t)n >= out_sz)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

/* The .shot header for a w x h frame (16 bytes, little-endian u16s). */
static inline void psp_state_shot_header(unsigned char hdr[16], unsigned w,
                                         unsigned h)
{
   memset(hdr, 0, 16);
   memcpy(hdr, PSP_STATE_THUMB_MAGIC, 4);
   hdr[4] = (unsigned char)(w & 0xFF);  hdr[5] = (unsigned char)(w >> 8);
   hdr[6] = (unsigned char)(h & 0xFF);  hdr[7] = (unsigned char)(h >> 8);
   hdr[8] = (unsigned char)(PSP_STATE_SHOT_STRIDE & 0xFF);
   hdr[9] = (unsigned char)(PSP_STATE_SHOT_STRIDE >> 8);
}

/* A .shot header the reader accepts: the magic, 0 < w <= 240, 0 < h <= 160,
 * stride 256.  Fills *w, *h; 0 = good. */
static inline int psp_state_shot_check(const unsigned char hdr[16],
                                       unsigned *w, unsigned *h)
{
   unsigned hw = hdr[4] | (unsigned)hdr[5] << 8;
   unsigned hh = hdr[6] | (unsigned)hdr[7] << 8;
   unsigned hs = hdr[8] | (unsigned)hdr[9] << 8;
   if (memcmp(hdr, PSP_STATE_THUMB_MAGIC, 4) != 0 || !hw ||
       hw > PSP_STATE_SHOT_MAX_W || !hh || hh > PSP_STATE_SHOT_MAX_H ||
       hs != PSP_STATE_SHOT_STRIDE)
      return -1;
   if (w) *w = hw;
   if (h) *h = hh;
   return 0;
}

/* DELETE (docs/CONTROL-REMAP.md section 11): the three files a delete may
 * remove, slot `slot`'s state and its two previews (.thumb, .shot), derived
 * from the SLOT-1 path alone.  The derivation is deliberately narrower than
 * psp_state_path_for_slot: `slot1_path` must already end in ".st0" (any case
 * of "st") after a non-empty name, and the state path is that string with
 * the final digit replaced -- so nothing a caller passes, a ROM path or a
 * .sav included, can make this name a file that is not a .stN, its
 * .stN.thumb or its .stN.shot.  Returns 0 and fills all three, or -1 with
 * all three left empty. */
static inline int psp_state_delete_paths(char *state, size_t state_sz,
                                         char *thumb, size_t thumb_sz,
                                         char *shot, size_t shot_sz,
                                         const char *slot1_path,
                                         unsigned slot)
{
   size_t n;
   if (state && state_sz)
      state[0] = '\0';
   if (thumb && thumb_sz)
      thumb[0] = '\0';
   if (shot && shot_sz)
      shot[0] = '\0';
   if (!state || !thumb || !shot || !slot1_path || slot < 1 ||
       slot > PSP_STATE_SLOT_COUNT)
      return -1;
   n = strlen(slot1_path);
   if (n < 5 || n + 1 > state_sz)
      return -1;
   if (slot1_path[n - 4] != '.' ||
       (slot1_path[n - 3] | 0x20) != 's' ||
       (slot1_path[n - 2] | 0x20) != 't' || slot1_path[n - 1] != '0' ||
       slot1_path[n - 5] == '/' || slot1_path[n - 5] == ':')
      return -1;
   memcpy(state, slot1_path, n + 1);
   state[n - 1] = (char)('0' + (slot - 1));
   if (psp_state_thumb_path(thumb, thumb_sz, state) != 0 ||
       psp_state_shot_path(shot, shot_sz, state) != 0)
   {
      state[0] = '\0';
      thumb[0] = '\0';
      shot[0] = '\0';
      return -1;
   }
   return 0;
}

/* THE LIST WINDOW (docs/UI-OVERLAY.md section 15.2): the first row (0-based
 * slot index) a list of `n` slots shows in `rows` rows, with the cursor on
 * slot `cur` (1-based) and the window last at `top`.  The cheats list's rule
 * (ui_psp.c g_cht_top): the window stays put while the cursor is in it and
 * otherwise moves just far enough to show it -- so a wrap from slot 1 to slot
 * 10 shows 6-10, and walking back up scrolls a row at a time.  With n <=
 * rows it is always 0, the five-slot page exactly.  Pure, so the host test
 * (tools/tests/test_state_slots.c) runs the code both lists run. */
static inline int psp_state_slot_top(int cur, int top, int n, int rows)
{
   if (cur > n)
      cur = n;
   if (cur < 1)
      cur = 1;
   if (cur - 1 < top)
      top = cur - 1;
   if (cur - 1 >= top + rows)
      top = cur - rows;
   if (top > n - rows)
      top = n - rows;
   if (top < 0)
      top = 0;
   return top;
}

#endif
