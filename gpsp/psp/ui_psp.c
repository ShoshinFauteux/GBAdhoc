/* ui_psp.c — native UI v2.
 *
 * v1 was a debug-grade panel menu.  v2 is the release face of the build:
 * a themed full-screen UI (gradient page, header/footer bars, accent
 * selection), a game-gallery ROM browser with box art, and a sectioned
 * settings screen.  The CONTRACT with main_psp.c is unchanged: same
 * ui_action enum, same entry points, same demo hooks — the overhaul is
 * confined to presentation and to one new config bit (osd_wireless).
 *
 * BOX ART: `<appdir>/boxart/<rom-name-minus-extension>.bmp`, uncompressed
 * 24/32-bpp BMP, any size (nearest-resampled to 112x112 at load).  BMP,
 * not PNG, deliberately: this tree carries no inflate/PNG code and the
 * GE eats raw RGB565 directly.  A ROM with no art gets a styled template
 * card, so a mixed folder still looks intentional.  Art is cached in a
 * small malloc'd pool that exists ONLY while the browser is on screen —
 * the in-game UI never touches it, so session RAM is untouched.
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>

#include "ui_psp.h"
#include "mgift_net.h"
#include "video_psp.h"
#include "stb_image.h"
#include "osd_psp.h"
#include "config_psp.h"
#include "rom_paths.h"
#include "state_slots.h"
#include "font_8x16.h"
#include "fe_favs.h"
#include "fe_evt.h"
#include "fe_host.h"
#include "transport_adhoc.h"
#include "ambient_look.h"
#include <psppower.h>   /* the header battery (ov_battery) */
#include "room_label.h"
#include "link_games.h"
#include "fe_linkmode.h"
#include "fe_gblink.h"      /* FE_GBLINK_FL_*: Fast link, as a session runs it */
#ifdef GPSP_CHEATS
#include "fe_cheats.h"
#endif

/* ----- theme (libretro RGB565) ------------------------------------------- */
/* Themed palette (RGB565, R in the high bits — rgb565_to_abgr converts for
 * the GE).  Every draw call goes through the C_* macros, so pointing g_thm
 * at a different palette reskins the entire UI in one assignment; page()
 * re-reads g_pcfg.theme each frame, which is what makes the Settings toggle
 * apply live.  The harness's black-background override (g_theme_black) sits
 * above this and only claims the page background.
 *
 * `title` is the header-bar text (both themes keep a dark header, so it
 * stays white); `sel` is selected-row/selected-card text, which must flip
 * to near-black on the light page — the one place the two roles diverge. */
typedef struct {
   uint16_t bg_top, bg_bot, hdr_top, hdr_bot, accent, accent_dk,
            title, sel, item, dim, value, warn, card, shadow;
} ui_theme;

/* Backgrounds are FLAT.  A full-screen ramp has only 32 steps of blue to
 * spend at RGB565, so it bands; with the GE dither that used to be on it
 * stippled as well.  Depth comes from translucent surfaces over a solid
 * field instead -- the same call RetroShell PSP makes. */
/* Monochrome by design: greys carry structure, and the ONE saturated
 * thing on screen is the box art.  A coloured chrome competes with it. */
/* GREY MEANS g6 == r5 * 2, ALWAYS.
 *
 * RGB565 gives green SIX bits and red/blue five, so the obvious encoding of a
 * grey -- r=v>>3, g=v>>2, b=v>>3 -- puts green a fraction higher than red and
 * blue once the hardware expands the channels back out.  It is invisible in
 * the midtones and clearly green in the darkest ones: #0E0E0E encoded that way
 * decodes to #080C08, a +4 green cast, and a PSP Go's panel shows it.
 *
 * So the darks are written as exact neutrals (g6 = 2 * r5) rather than as the
 * closest encoding of a hex triple.  Black, dark grey, white accents. */
static const ui_theme THM_DARK = {
   0x0000, 0x0000,           /* pure black field                          */
   0x1082, 0x1082,           /* #101010 header bar   (r5 2, g6 4, b5 2)   */
   0xFFFF, 0x8C51,           /* white accent / #8B8B8B dim accent         */
   0xFFFF, 0xFFFF,           /* header title / selected text: white       */
   0xCE59, 0x6B4D, 0xEF5D,   /* #C9C9C9 item / #6B6B6B dim / #E8E8E8 value*/
   0xFFFF,                   /* warn: white -- the words carry the alarm  */
   0x18C3, 0x0000            /* #181818 card body / black shadow          */
};

static const ui_theme THM_LIGHT = {
   0xF79E, 0xF79E,           /* #F2F2F0 flat off-white                    */
   0xE71C, 0xE71C,           /* #E3E3E1 header bar                        */
   0x0000, 0x6B4D,           /* black accent / #6B6B6B dim accent         */
   0x0000, 0x0000,           /* header title / selected text: black       */
   0x2945, 0x8C71, 0x1082,   /* #2B2B2B item / #8C8C8C dim / #101010 value*/
   0x0000,                   /* warn                                      */
   0xFFFF, 0xAD55            /* white card body / #A8A8A8 shadow (neutral)*/
};

static const ui_theme *g_thm = &THM_DARK;

#define C_BG_TOP    (g_thm->bg_top)
#define C_BG_BOT    (g_thm->bg_bot)
#define C_HDR_TOP   (g_thm->hdr_top)
#define C_HDR_BOT   (g_thm->hdr_bot)
#define C_ACCENT    (g_thm->accent)
#define C_ACCENT_DK (g_thm->accent_dk)
#define C_TITLE     (g_thm->title)
#define C_SEL       (g_thm->sel)
#define C_ITEM      (g_thm->item)
#define C_DIM       (g_thm->dim)
#define C_VALUE     (g_thm->value)
#define C_WARN      (g_thm->warn)
#define C_CARD      (g_thm->card)
#define C_SHADOW    (g_thm->shadow)

/* ----- console skins ------------------------------------------------------ */
/* The chrome stays monochrome (see above: the box art is the one saturated
 * thing on screen).  A console gets ONE colour, spent in exactly three
 * places -- the header badge, the 3 px selection edge, and the favourite
 * star -- plus a palette that appears only on the badge's 2 px stripe and
 * in the TRIANGLE flare.  GBA is its indigo shell; GB is the DMG's four
 * green-grey shades, which are also the palette a GB game draws in; GBC is
 * the five translucent shell colours, with teal as the accent because
 * Atomic Purple sits too close to the GBA indigo to read as a change.
 *
 * Each has a light-theme accent: the dark ones vanish on the off-white page.
 * Indexed by fe_console_t: GBA, GB, GBC. */
#define RGB565(r, g, b) \
   ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

typedef struct {
   const char *id;          /* "GBA" -- the badge and the flare caption    */
   const char *full;        /* "Game Boy Advance" -- the flare caption     */
   const char *ext;         /* "gba" -- the empty-state message            */
   uint16_t    accent;      /* badge text, selection edge, star            */
   uint16_t    pal[5];      /* badge stripe and flare bands, in order      */
   int         pal_n;
   int         cart_wide;   /* GBA carts are wider than tall; GB/GBC tall  */
} ui_skin;

static const ui_skin SKIN_DARK[FE_CONSOLE_COUNT] = {
   { "GBA", "Game Boy Advance", "gba", RGB565(0x8F, 0x80, 0xFF),
     { RGB565(0x2E, 0x24, 0x72), RGB565(0x5B, 0x4B, 0xC4),
       RGB565(0x8F, 0x80, 0xFF), 0, 0 }, 3, 1 },
   { "GB",  "Game Boy",         "gb",  RGB565(0x9B, 0xBC, 0x0F),
     { RGB565(0x0F, 0x38, 0x0F), RGB565(0x30, 0x62, 0x30),
       RGB565(0x8B, 0xAC, 0x0F), RGB565(0x9B, 0xBC, 0x0F), 0 }, 4, 0 },
   { "GBC", "Game Boy Color",   "gbc", RGB565(0x2F, 0xD6, 0xC8),
     { RGB565(0xE8, 0x38, 0x6D), RGB565(0xF5, 0xC5, 0x18),
       RGB565(0x7D, 0xCB, 0x2F), RGB565(0x1F, 0xB8, 0xB0),
       RGB565(0x7B, 0x4F, 0xD8) }, 5, 0 },
};

/* Light accents are DARK: they are read as text on the badge's pale plate
 * (~#DDDAF4 for GBA), and 4.5:1 there needs a colour around L* 30. */
static const ui_skin SKIN_LIGHT[FE_CONSOLE_COUNT] = {
   { "GBA", "Game Boy Advance", "gba", RGB565(0x3B, 0x2D, 0xA4),
     { RGB565(0x2E, 0x24, 0x72), RGB565(0x5B, 0x4B, 0xC4),
       RGB565(0x8F, 0x80, 0xFF), 0, 0 }, 3, 1 },
   { "GB",  "Game Boy",         "gb",  RGB565(0x2E, 0x56, 0x12),
     { RGB565(0x0F, 0x38, 0x0F), RGB565(0x30, 0x62, 0x30),
       RGB565(0x8B, 0xAC, 0x0F), RGB565(0x9B, 0xBC, 0x0F), 0 }, 4, 0 },
   { "GBC", "Game Boy Color",   "gbc", RGB565(0x0A, 0x6C, 0x66),
     { RGB565(0xE8, 0x38, 0x6D), RGB565(0xF5, 0xC5, 0x18),
       RGB565(0x7D, 0xCB, 0x2F), RGB565(0x1F, 0xB8, 0xB0),
       RGB565(0x7B, 0x4F, 0xD8) }, 5, 0 },
};

static const ui_skin *skin_of(int console)
{
   if (console < FE_CONSOLE_GBA || console >= FE_CONSOLE_COUNT)
      console = FE_CONSOLE_GBA;
   return (g_pcfg.theme == 1 ? SKIN_LIGHT : SKIN_DARK) + console;
}

/* The active console's accent; the browser's fourth colour macro. */
#define C_CON (skin_of(g_pcfg.console)->accent)

/* ----- the header row: browser shells and the loading screen -----------
 * ONE geometry, so the wordmark and the console card never move between
 * the browser and ROM load (docs/UI-OVERLAY.md §12.3).  The card is the
 * rightmost thing in the row, on the header's 460 margin; the browser keeps
 * its TRIANGLE hint left of it (TRIANGLE switches console) and the loading
 * screen keeps its busy ring in that place.
 * The wordmark's 24 rows are centred on the card's 20: 7 + 12 = 9 + 10. */
#define HDR_LOGO_X   20
#define HDR_LOGO_Y   7
#define HDR_CARD_R   460     /* the card's right edge                      */
#define HDR_CARD_Y   9
#define HDR_CARD_H   20
#define HDR_TEXT_Y   11      /* a line of vid_text in the header row       */
#define HDR_RING_DX  13      /* ring centre, left of the card's left edge  */

static int hdr_card_w(const ui_skin *s)
{
   return 8 + vid_text_w(s->id) + 8;
}

/* The console card: a translucent plate in the console colour, its name,
 * and the palette as a 2 px stripe along the bottom.  `scale` 256 = 1:1;
 * the TRIANGLE flare lands it at 1.35x (g_fx_badge).  Returns the card's
 * left edge at 1:1, which is where the TRIANGLE hint / busy ring hang. */
static int hdr_card(const ui_skin *s, int scale)
{
   int bw = hdr_card_w(s), x = HDR_CARD_R - bw, k;
   int sw = (bw * scale) >> 8, sh = (HDR_CARD_H * scale) >> 8;
   int bx = x + bw / 2 - sw / 2, by = HDR_CARD_Y + HDR_CARD_H / 2 - sh / 2;
   vid_rect(bx, by, sw, sh, s->accent, g_pcfg.theme == 1 ? 36 : 48);
   for (k = 0; k < s->pal_n; k++)
      vid_rect(bx + (k * sw) / s->pal_n, by + sh - 2,
               ((k + 1) * sw) / s->pal_n - (k * sw) / s->pal_n, 2,
               s->pal[k], 255);
   vid_text(x + 8, HDR_TEXT_Y, s->id, s->accent);
   return x;
}

/* The busy ring, eight dots chasing at 10 Hz, in the TRIANGLE hint's place
 * left of the card. */
static void hdr_ring(int card_x, unsigned now, uint16_t col)
{
   static const signed char ring[8][2] = {
      {0,-7}, {5,-5}, {7,0}, {5,5}, {0,7}, {-5,5}, {-7,0}, {-5,-5}
   };
   int i, cx = card_x - HDR_RING_DX, cy = HDR_CARD_Y + HDR_CARD_H / 2;
   for (i = 0; i < 8; i++)
      vid_rect(cx + ring[i][0] - 1, cy + ring[i][1] - 1, 3, 3, col,
               50 + 25 * ((i + 8 - (now / 100000) % 8) % 8));
}

/* "Quit to game list": the hold and the crossfade (ui_return_*, below). */
static void ret_scan_tick(void);
static int  g_ret_on;

/* a -> b by t/256, per channel.  The GE has no text alpha (the font is a
 * coverage texture modulated by an opaque vertex colour), so text that has
 * to fade is drawn in a colour walked toward the background instead. */
static uint16_t mix565(uint16_t a, uint16_t b, int t)
{
   int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
   int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
   if (t <= 0) return a;
   if (t >= 256) return b;
   ar += ((br - ar) * t) >> 8;
   ag += ((bg - ag) * t) >> 8;
   ab += ((bb - ab) * t) >> 8;
   return (uint16_t)((ar << 11) | (ag << 5) | ab);
}

/* ----- state -------------------------------------------------------------- */
enum { SCR_MENU, SCR_SETTINGS, SCR_WIRELESS, SCR_SCAN, SCR_MGIFT,
       SCR_STATE_SLOTS, SCR_CONTROLS, SCR_HOST, SCR_JOINASK, SCR_CHEATS };

static int g_active;
static int g_screen;
static int g_cursor;
static int g_settings_dirty;
static int g_set_scroll;      /* SETTINGS list scroll, px (see set_row_y) */
/* Browser Settings: 1 = Share game data, 2 = the room list was chosen. */
static int g_share_req;
static char g_state_base[PSP_FILE_PATH_CAP];
static int g_state_slot = 1;
static int g_browser_state_open;
static int g_ui_shell;                    /* 0 = Shelf, 1 = Marquee */
static int g_browser_state_slot = 1;
/* Shelf delete (docs/CONTROL-REMAP.md section 11): the slot whose delete is
 * waiting for X (0 = none), and the panel's one-line report of the last one,
 * kept until the next press. */
static int  g_shelf_del;
static char g_shelf_note[32];
/* State-shelf slide, 0 (closed) .. PANEL_FRAMES (open), eased at draw time.
 * It was a linear 26 px/frame step that also stat()ed all five slots and
 * read a thumbnail from the stick on the same frames -- Memory Stick I/O
 * inside an animation is what made it stutter. */
#define PANEL_FRAMES 11
static int g_browser_panel_t;
/* Which slots have a state file, filled once by browser_rom_has_state()
 * when the shelf opens, so drawing never touches the stick. */
static unsigned char g_browser_state_exists[PSP_STATE_SLOT_COUNT];
static uint16_t *g_browser_previews;
static unsigned char g_browser_preview_status[PSP_STATE_SLOT_COUNT];
static int g_browser_preview_rom = -1;

static unsigned g_prev_pad;
static int g_rep_timer;

static char g_join_group[9];

/* ---- the overlay's state (docs/UI-OVERLAY.md; drawn by ov_*, below) ---- */
#define OV_OPEN_FRAMES 8       /* the menu's opening transition          */
#define OV_SCRIM       170     /* menu, slots, wireless                  */
#define OV_SCRIM_DENSE 200     /* settings, controls: long lists         */
#define OV_RAMP_Y      (VID_SCR_H - 120)
#define OV_PLATE_Y     (VID_SCR_H - FTR_H)

static const uint16_t *g_ov_frame;          /* the snapshot, or NULL      */
static int   g_ov_fw = 240, g_ov_fh = 160;   /* its size in texels         */
static uint16_t *g_ov_full;                 /* harness: 480x272 at 512    */
static uint16_t *g_ov_thumbs;               /* 5 x 64x64, or NULL         */
static int   g_ov_browse;                   /* browser Settings: art bake */
static int   g_ov_open_k = OV_OPEN_FRAMES;   /* frames into the transition */
static int   g_ov_linked;                   /* a session is up            */
static char  g_ov_title[100];               /* the game's name            */
/* Per slot: 0 no state, 1 a state (no preview), 2 a state and its preview.
 * Filled ONCE when the slots screen opens -- never per frame: a stat per
 * slot per frame is Memory Stick I/O inside the draw loop. */
static unsigned char g_ov_slot[PSP_STATE_SLOT_COUNT];
/* Each state's time, "3 Oct 2026, 14:32", from the same stat (st_mtime): no
 * extra I/O and no new bytes on the card.  "" = empty slot. */
static char g_ov_stamp[PSP_STATE_SLOT_COUNT][24];
static int  g_ov_used;                      /* slots holding a state      */
/* THE SAVE STATES PAGE (docs/UI-OVERLAY.md section 13).  A question on the
 * plate -- 0 none, SLOT_ASK_DELETE or SLOT_ASK_OVERWRITE -- for the slot
 * g_slot_ask_n; while it is up X answers it and any other key keeps the
 * slot.  The plate that reports what happened is cleared by the next key. */
enum { SLOT_ASK_DELETE = 1, SLOT_ASK_OVERWRITE = 2 };
static int  g_slot_ask;
static int  g_slot_ask_n;
static char g_slot_note[48];
static int  g_slot_saving;                  /* slot a UI_ACT_SAVESTATE is for */
static int  g_slots_dirty;                  /* rescan on the next frame   */
/* The full preview: ONE slot's .shot, read lazily into the VRAM texture
 * (vid_shot_tex) once the cursor has rested OV_SHOT_DWELL frames -- more
 * than the d-pad's 5-frame repeat, so a held d-pad never reads. */
#define OV_SHOT_DWELL 6
static int  g_ov_shot_slot = -1;            /* slot index the texture holds */
static int  g_ov_shot_w, g_ov_shot_h;       /* 240x160 or 160x144          */
static int  g_ov_dwell;                     /* frames the cursor has rested */
/* A read was tried and there is no usable .shot (a 3.1 state): the panel
 * says so under the 2x thumbnail.  Reset by every scan. */
static unsigned char g_ov_shot_none[PSP_STATE_SLOT_COUNT];
static int  g_shot_probe;                   /* harness: price the read once */



void ui_set_state_base(const char *slot1_path)
{
   if (slot1_path)
      snprintf(g_state_base, sizeof(g_state_base), "%s", slot1_path);
   else
      g_state_base[0] = '\0';
}

int ui_state_slot(void)
{
   return g_state_slot;
}

/* scan results */
static char g_scan_groups[8][9];
static int  g_scan_count = -1;   /* -1 = not scanned yet */

/* ---- link mode (docs/GBA-LINK-UI.md) -------------------------------------
 * The running game as the Wireless menus need it (ui_set_game), the
 * per-game override file (ui_linkmode_load; BSS, like the favourites), and
 * the session as main_psp.c reports it (ui_set_link). */
static char g_game_code[5];          /* GBA header code, "" on GB/GBC     */
static char g_game_gbtitle[17];      /* GB cartridge title                 */
static char g_game_key[FE_LINKMODE_NAME];  /* linkmode.txt key             */
static int  g_cable_ready;           /* this boot can host by cable        */
static fe_linkmode g_lm;             /* linkmode.txt, ~13 KB BSS           */
static char g_lm_dir[144];           /* where it lives (roms/)             */
static int  g_lm_loaded;
static int  g_host_cable;            /* the last Host's mode               */
static ui_link_view g_link;          /* the session, per frame             */
static char g_link_room[ROOM_LABEL_CAP];
static char g_link_game[64];
static char g_scan_note[96];         /* the scan list's plate, or ""       */
static char g_ask_group[2][9];       /* Join room N found both: cable, adapter */
/* Browser Settings: the highlighted game's key and code (read once when
 * the page opens; "" = no game highlighted). */
static char g_bset_key[FE_LINKMODE_NAME];
static char g_bset_code[5];
static int  g_bset_have;

/* main_psp.c: re-applies the session chip after the OSD toggle changes. */
extern void osd_session_chip_refresh(void);

/* ----- demo (harness self-drive) ------------------------------------------ */
typedef struct { unsigned pad; unsigned char hold, gap; } demo_step;
#define DEMO_DUMP 0xFFFFFFFFu
static const demo_step demo_script[] = {
   { 0,               30, 0 },   /* settle on menu     */
   { DEMO_DUMP,        1, 0 },   /* ge_ui_0: the menu  */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states     */
   { PSP_CTRL_CROSS,   2, 5 },   /* open the page (slot 1) */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> slot 2          */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> slot 3          */
   { 0,               10, 0 },   /* rest: the dwell's read lands */
   { DEMO_DUMP,        1, 0 },   /* ge_ui_1: slot 3 (fixtures: .thumb only) */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> slot 4 (empty)  */
   { DEMO_DUMP,        1, 0 },   /* ge_ui_2: the empty slot */
   { PSP_CTRL_SQUARE,  2, 14 },  /* save slot 4: the page stays */
   { DEMO_DUMP,        1, 0 },   /* ge_ui_3: "Slot 4 saved", its picture */
   { PSP_CTRL_UP,      2, 4 },   /* -> slot 3          */
   { PSP_CTRL_TRIANGLE, 2, 4 },  /* delete?            */
   { DEMO_DUMP,        1, 0 },   /* ge_ui_4: the delete question */
   { PSP_CTRL_CIRCLE,  2, 4 },   /* keep it            */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> slot 4          */
   { PSP_CTRL_SQUARE,  2, 4 },   /* overwrite?         */
   { DEMO_DUMP,        1, 0 },   /* ge_ui_5: the overwrite question */
   { PSP_CTRL_CIRCLE,  2, 4 },   /* keep it            */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to the menu (cursor -> Resume) */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states     */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Wireless        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Settings        */
   { PSP_CTRL_CROSS,   2, 8 },   /* enter settings     */
   /* Settings now starts at Room; move through Link mode and Session
    * overlay to Video scale and cycle through all three scale modes. */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Link mode       */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Session overlay */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Video scale     */
   { PSP_CTRL_RIGHT,   2, 4 },   /* cycle scale        */
   { DEMO_DUMP,        1, 0 },   /* ge_ui_6: settings  */
   { PSP_CTRL_RIGHT,   2, 4 },
   { PSP_CTRL_RIGHT,   2, 4 },
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to menu (cursor -> Resume) */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states     */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Wireless        */
   { PSP_CTRL_CROSS,   2, 8 },   /* enter wireless     */
   { DEMO_DUMP,        1, 0 },   /* ge_ui_7: wireless  */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back (cursor -> Resume) */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states     */
   { PSP_CTRL_CROSS,   2, 5 },   /* open the page (slot 1) */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> slot 2          */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> slot 3          */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> slot 4, saved above */
   { PSP_CTRL_CROSS,   2, 10 },  /* load it: the menu closes (demo ends) */
};
/* ui_controls_demo=1: Settings > Controls end to end (docs/CONTROL-REMAP.md).
 * Cursor arithmetic assumes the harness build's table (no catch self-test
 * row).  Every step is a real pad press through the same capture code a
 * player's thumb drives. */
static const demo_step ctl_demo_script[] = {
   { 0,               30, 0 },   /* settle on menu                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Wireless                           */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Settings                           */
   { PSP_CTRL_CROSS,   2, 8 },   /* enter settings (cursor: Room)    */
   { PSP_CTRL_UP,      2, 6 },   /* wraps to the last row: Controls       */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_0: Settings, Controls row      */
   { PSP_CTRL_CROSS,   2, 10 },  /* open Controls (cursor: A)             */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_1: the default table           */
   { PSP_CTRL_DOWN,    2, 4 },   /* B                                     */
   { PSP_CTRL_DOWN,    2, 4 },   /* L                                     */
   { PSP_CTRL_DOWN,    2, 4 },   /* R                                     */
   { PSP_CTRL_DOWN,    2, 4 },   /* Start                                 */
   { PSP_CTRL_DOWN,    2, 4 },   /* Select                                */
   { PSP_CTRL_DOWN,    2, 4 },   /* Up                                    */
   { PSP_CTRL_DOWN,    2, 4 },   /* Down                                  */
   { PSP_CTRL_DOWN,    2, 4 },   /* Left                                  */
   { PSP_CTRL_DOWN,    2, 4 },   /* Right                                 */
   { PSP_CTRL_DOWN,    2, 4 },   /* Turbo buttons (the row to its page)   */
   { PSP_CTRL_DOWN,    2, 4 },   /* Fast-forward (top of SHORTCUTS)       */
   { PSP_CTRL_DOWN,    2, 4 },   /* Video preset                          */
   { PSP_CTRL_SQUARE,  2, 8 },   /* unbind it: Triangle no longer resizes */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_2: Video preset = none         */
   { PSP_CTRL_DOWN,    2, 4 },   /* Quick save                            */
   { PSP_CTRL_DOWN,    2, 4 },   /* Quick load                            */
   { PSP_CTRL_DOWN,    2, 4 },   /* Screenshot                            */
   { PSP_CTRL_CROSS,   2, 12 },  /* capture...                            */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_3: the capture prompt          */
   { PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER, 8, 10 },  /* a chord: L+R    */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_4: Screenshot = L+R            */
   { PSP_CTRL_UP,      2, 4 },   /* Quick load                            */
   { PSP_CTRL_UP,      2, 4 },   /* Quick save                            */
   { PSP_CTRL_UP,      2, 4 },   /* Video preset                          */
   { PSP_CTRL_UP,      2, 4 },   /* Fast-forward                          */
   { PSP_CTRL_UP,      2, 4 },   /* Turbo buttons                         */
   { PSP_CTRL_UP,      2, 4 },   /* Right                                 */
   { PSP_CTRL_UP,      2, 4 },   /* Left                                  */
   { PSP_CTRL_UP,      2, 4 },   /* Down                                  */
   { PSP_CTRL_UP,      2, 4 },   /* Up                                    */
   { PSP_CTRL_UP,      2, 4 },   /* Select                                */
   { PSP_CTRL_UP,      2, 4 },   /* Start                                 */
   { PSP_CTRL_UP,      2, 4 },   /* R                                     */
   { PSP_CTRL_UP,      2, 4 },   /* L                                     */
   { PSP_CTRL_UP,      2, 4 },   /* B                                     */
   { PSP_CTRL_UP,      2, 4 },   /* A                                     */
   { PSP_CTRL_CROSS,   2, 12 },  /* capture A...                          */
   { PSP_CTRL_SQUARE,  4, 10 },  /* ...Square: steals Fast-forward        */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_5: the steal, and its note     */
   { PSP_CTRL_UP,      2, 6 },   /* wraps to Reset (fixed rows skipped)   */
   { PSP_CTRL_CROSS,   2, 6 },   /* arm                                   */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_6: confirm prompt              */
   { PSP_CTRL_CROSS,   2, 8 },   /* reset                                 */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_7: back to the defaults        */
   /* The design's two states, exactly (docs/UI-OVERLAY.md): capture on
    * Fast-forward, then the conflict when Triangle is taken from another
    * row.  Then both rows are put back. */
   { PSP_CTRL_DOWN,    2, 4 },   /* wraps to A                            */
   { PSP_CTRL_RIGHT,   2, 4 },   /* across the columns: Fast-forward      */
   { PSP_CTRL_CROSS,   2, 12 },  /* capture Fast-forward...               */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_8: the capture chip and plate  */
   { PSP_CTRL_TRIANGLE, 4, 10 }, /* ...Triangle: taken from Video preset  */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_9: the conflict plate          */
   { PSP_CTRL_TRIANGLE, 2, 6 },  /* Fast-forward back to its default      */
   { PSP_CTRL_DOWN,    2, 4 },   /* Video preset (the plate is read)      */
   { PSP_CTRL_TRIANGLE, 2, 6 },  /* back to Triangle                      */
   { PSP_CTRL_LEFT,    2, 4 },   /* across, same row: B                   */
   { DEMO_DUMP,        1, 0 },   /* ge_ctl_10: defaults, cursor on B      */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to Settings                      */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to the menu (cursor: Resume)     */
   { PSP_CTRL_CROSS,   2, 8 },   /* resume                                */
};

/* ui_controls_demo=3: the menu-button toggle (docs/CONTROL-REMAP.md section
 * 10).  From A, UP wraps to Reset and UP again lands on Menu: the fixed
 * Home/Quit row between them is not a stop. */
static const demo_step cth_demo_script[] = {
   { 0,               30, 0 },   /* settle on menu                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Wireless                           */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Settings                           */
   { PSP_CTRL_CROSS,   2, 8 },   /* enter settings                        */
   { PSP_CTRL_UP,      2, 6 },   /* wraps to Controls                     */
   { PSP_CTRL_CROSS,   2, 10 },  /* open Controls (cursor: A)             */
   { PSP_CTRL_UP,      2, 4 },   /* wraps to Reset                        */
   { PSP_CTRL_UP,      2, 4 },   /* Menu (Home/Quit is not a stop)        */
   { DEMO_DUMP,        1, 0 },   /* ge_cth_0: the value on the card       */
   { PSP_CTRL_CROSS,   2, 8 },   /* flip it                               */
   { DEMO_DUMP,        1, 0 },   /* ge_cth_1: flipped, and its plate      */
   { PSP_CTRL_DOWN,    2, 4 },   /* Reset (the plate is read)             */
   { PSP_CTRL_UP,      2, 4 },   /* Menu again                            */
   { DEMO_DUMP,        1, 0 },   /* ge_cth_2: the footer for this row     */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to Settings: saves               */
   { DEMO_DUMP,        1, 0 },   /* ge_cth_3: Button mapping custom/default */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to the menu                      */
   { PSP_CTRL_CROSS,   2, 8 },   /* resume                                */
};

/* ui_controls_demo=4: delete a save state in game (section 11). */
static const demo_step del_demo_script[] = {
   { 0,               30, 0 },   /* settle on menu                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states                        */
   { PSP_CTRL_CROSS,   2, 8 },   /* open the page (cursor: slot 1)        */
   { DEMO_DUMP,        1, 0 },   /* ge_del_0: empty slot 1, no triangle   */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> slot 2                             */
   { PSP_CTRL_SQUARE,  2, 12 },  /* save slot 2 (the page stays)          */
   { DEMO_DUMP,        1, 0 },   /* ge_del_1: slot 2 saved, its plate     */
   { PSP_CTRL_TRIANGLE, 2, 6 },  /* ask                                   */
   { DEMO_DUMP,        1, 0 },   /* ge_del_2: the question on the plate   */
   { PSP_CTRL_CIRCLE,  2, 6 },   /* keep it: O answers, it does not leave */
   { DEMO_DUMP,        1, 0 },   /* ge_del_3: still saved, still here     */
   { PSP_CTRL_TRIANGLE, 2, 6 },  /* ask again                             */
   { PSP_CTRL_CROSS,   2, 10 },  /* delete                                */
   { DEMO_DUMP,        1, 0 },   /* ge_del_4: slot 2 empty, the report    */
   { PSP_CTRL_TRIANGLE, 2, 6 },  /* on an empty slot: nothing to ask      */
   { DEMO_DUMP,        1, 0 },   /* ge_del_5: no question                 */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to the menu                      */
   { PSP_CTRL_CROSS,   2, 8 },   /* resume                                */
};

/* ui_controls_demo=6: the Turbo buttons page (docs/CONTROL-REMAP.md section
 * 13).  Turbo A goes on Triangle at 30 a second, Toggle; Turbo B on Square,
 * left at 15 and Hold; both steals are said on the plate. */
static const demo_step tb_demo_script[] = {
   { 0,               30, 0 },   /* settle on menu                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Wireless                           */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Settings                           */
   { PSP_CTRL_CROSS,   2, 8 },   /* enter settings                        */
   { PSP_CTRL_UP,      2, 6 },   /* wraps to Controls                     */
   { PSP_CTRL_CROSS,   2, 10 },  /* open Controls (cursor: A)             */
   { PSP_CTRL_DOWN,    2, 4 },   /* B                                     */
   { PSP_CTRL_DOWN,    2, 4 },   /* L                                     */
   { PSP_CTRL_DOWN,    2, 4 },   /* R                                     */
   { PSP_CTRL_DOWN,    2, 4 },   /* Start                                 */
   { PSP_CTRL_DOWN,    2, 4 },   /* Select                                */
   { PSP_CTRL_DOWN,    2, 4 },   /* Up                                    */
   { PSP_CTRL_DOWN,    2, 4 },   /* Down                                  */
   { PSP_CTRL_DOWN,    2, 4 },   /* Left                                  */
   { PSP_CTRL_DOWN,    2, 4 },   /* Right                                 */
   { PSP_CTRL_DOWN,    2, 4 },   /* Turbo buttons                         */
   { DEMO_DUMP,        1, 0 },   /* ge_tb_0: the row, nothing set         */
   { PSP_CTRL_CROSS,   2, 10 },  /* open the page (cursor: Turbo A)       */
   { DEMO_DUMP,        1, 0 },   /* ge_tb_1: all unbound, 15, Hold        */
   { PSP_CTRL_CROSS,   2, 12 },  /* capture Turbo A...                    */
   { DEMO_DUMP,        1, 0 },   /* ge_tb_2: the capture chip and plate   */
   { PSP_CTRL_TRIANGLE, 4, 10 }, /* ...Triangle: taken from Video preset  */
   { DEMO_DUMP,        1, 0 },   /* ge_tb_3: bound, and the conflict plate */
   { PSP_CTRL_RIGHT,   2, 4 },   /* -> its rate                           */
   { PSP_CTRL_CROSS,   2, 6 },   /* 15 -> 30                              */
   { DEMO_DUMP,        1, 0 },   /* ge_tb_4: the rate cell and its help   */
   { PSP_CTRL_RIGHT,   2, 4 },   /* -> its mode                           */
   { PSP_CTRL_CROSS,   2, 6 },   /* Hold -> Toggle                        */
   { DEMO_DUMP,        1, 0 },   /* ge_tb_5: the mode cell and its help   */
   { PSP_CTRL_DOWN,    2, 4 },   /* Turbo A (2)                           */
   { PSP_CTRL_DOWN,    2, 4 },   /* Turbo B                               */
   { PSP_CTRL_RIGHT,   2, 4 },   /* wraps to its button                   */
   { PSP_CTRL_CROSS,   2, 12 },  /* capture Turbo B...                    */
   { PSP_CTRL_SQUARE,  4, 10 },  /* ...Square: taken from Fast-forward    */
   { PSP_CTRL_DOWN,    2, 4 },   /* Turbo B (2) (the plate is read)       */
   { DEMO_DUMP,        1, 0 },   /* ge_tb_6: two set, each its own way    */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to Controls (cursor: the row)    */
   { DEMO_DUMP,        1, 0 },   /* ge_tb_7: "2 set", the two blank rows  */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to Settings: saves               */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to the menu                      */
   { PSP_CTRL_CROSS,   2, 8 },   /* resume                                */
};

/* ui_controls_demo=7 (REWIND builds, CONFIG.INI rewind = 1): the "Rewind
 * length" row of Settings in each of its three values.  From Room, UP wraps
 * to Button mapping, then Thumbstick, then (the CONTROLS header is not a
 * stop) Rewind length. */
static const demo_step rwl_demo_script[] = {
   { 0,               30, 0 },   /* settle on menu                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Wireless                           */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Settings                           */
   { PSP_CTRL_CROSS,   2, 8 },   /* enter settings                        */
   { PSP_CTRL_UP,      2, 6 },   /* wraps to Button mapping               */
   { PSP_CTRL_UP,      2, 6 },   /* Thumbstick                            */
   { PSP_CTRL_UP,      2, 6 },   /* Rewind length                         */
   { DEMO_DUMP,        1, 0 },   /* ge_rwl_0: the value on the card       */
   { PSP_CTRL_RIGHT,   2, 6 },
   { DEMO_DUMP,        1, 0 },   /* ge_rwl_1: the next                    */
   { PSP_CTRL_RIGHT,   2, 6 },
   { DEMO_DUMP,        1, 0 },   /* ge_rwl_2: the third                   */
   { PSP_CTRL_RIGHT,   2, 6 },   /* back to the first                     */
   { PSP_CTRL_UP,      2, 6 },   /* Rewind (the on/off row)               */
   { DEMO_DUMP,        1, 0 },   /* ge_rwl_3: the two rows, ordinary footer */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to the menu: saves               */
   { PSP_CTRL_CROSS,   2, 8 },   /* resume                                */
};

/* ui_controls_demo=2 (with browser=1): the same page reached from the ROM
 * browser's START settings, which run their own loop (browser_settings). */
static const demo_step ctl_bdemo_script[] = {
   { 0,               20, 0 },   /* settle on settings (cursor: Room) */
   { PSP_CTRL_UP,      2, 6 },   /* wraps to Controls                      */
   { DEMO_DUMP,        1, 0 },   /* ge_ctlb_0: browser settings            */
   { PSP_CTRL_CROSS,   2, 10 },  /* open Controls                          */
   { DEMO_DUMP,        1, 0 },   /* ge_ctlb_1: the page, from the browser  */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to settings                       */
   { PSP_CTRL_CIRCLE,  2, 8 },   /* back to the browser                    */
};

/* ui_controls_demo=5 / harness inproc_via_menu: the player's own way out,
 * Quit to game list from the menu (docs/INPROC-RETURN.md). */
static const demo_step quit_demo_script[] = {
   { 0,               20, 0 },   /* settle on menu                        */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Save states (one row since 3.5)    */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Wireless                           */
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Settings                           */
#ifdef GPSP_CHEATS
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Cheats                             */
#endif
   { PSP_CTRL_DOWN,    2, 4 },   /* -> Quit to game list                  */
   { DEMO_DUMP,        1, 0 },   /* ge_quit_0: the menu, on the row       */
   { PSP_CTRL_CROSS,   2, 4 },   /* quit                                  */
};

static const demo_step *g_demo_steps = demo_script;
static int g_demo_len = (int)(sizeof(demo_script) / sizeof(demo_script[0]));
static const char *g_demo_prefix = "ge_ui";
static int g_demo_on = -1;       /* -1 idle, else script index */
static int g_demo_dump_pending;  /* a DEMO_DUMP waits for the frame's draw */
static int g_demo_phase;         /* frames left in hold(+)/gap(-) */

/* Asset-shoot arm (README screenshots): when set, the ROM browser dumps the
 * gallery after the box art settles and auto-picks the current game, so a
 * ui_demo run needs no human input from cold boot to finished dump set. */
static int g_ui_shots;
void ui_demo_shots(void) { g_ui_shots = 1; }

/* Page background: black is the HARNESS identity, the slate gradient the
 * playable's.  Runtime-overridable (ui_theme_black=0) so the asset shoot can
 * photograph the playable look from the harness-channel build. */
#ifdef GPSP_PLAYABLE
static int g_theme_black = 0;
#else
static int g_theme_black = 1;
#endif
void ui_set_theme_black(int b) { if (b >= 0) g_theme_black = b ? 1 : 0; }

void ui_demo_start(void)
{
   g_demo_steps = demo_script;
   g_demo_len = (int)(sizeof(demo_script) / sizeof(demo_script[0]));
   g_demo_prefix = "ge_ui";
   g_demo_on = 0;
   g_demo_phase = 0;
   fe_evt("ui_demo_start");
}

static int g_ctl_bdemo;        /* 1 = armed for the browser, 2 = pick next */
void ui_controls_browser_demo(void) { g_ctl_bdemo = 1; }

static void ctl_bdemo_start(void)
{
   g_demo_steps = ctl_bdemo_script;
   g_demo_len = (int)(sizeof(ctl_bdemo_script) / sizeof(ctl_bdemo_script[0]));
   g_demo_prefix = "ge_ctlb";
   g_demo_on = 0;
   g_demo_phase = 0;
   fe_evt("ui_demo_start script=controls_browser");
}

void ui_controls_demo_start(void)
{
   g_demo_steps = ctl_demo_script;
   g_demo_len = (int)(sizeof(ctl_demo_script) / sizeof(ctl_demo_script[0]));
   g_demo_prefix = "ge_ctl";
   g_demo_on = 0;
   g_demo_phase = 0;
   fe_evt("ui_demo_start script=controls");
}

void ui_home_demo_start(int which)
{
   if (which == 7)
   {
      g_demo_steps = rwl_demo_script;
      g_demo_len = (int)(sizeof(rwl_demo_script) / sizeof(rwl_demo_script[0]));
      g_demo_prefix = "ge_rwl";
   }
   else if (which == 6)
   {
      g_demo_steps = tb_demo_script;
      g_demo_len = (int)(sizeof(tb_demo_script) / sizeof(tb_demo_script[0]));
      g_demo_prefix = "ge_tb";
   }
   else if (which == 5)
   {
      g_demo_steps = quit_demo_script;
      g_demo_len = (int)(sizeof(quit_demo_script) /
                         sizeof(quit_demo_script[0]));
      g_demo_prefix = "ge_quit";
   }
   else if (which == 4)
   {
      g_demo_steps = del_demo_script;
      g_demo_len = (int)(sizeof(del_demo_script) / sizeof(del_demo_script[0]));
      g_demo_prefix = "ge_del";
   }
   else
   {
      g_demo_steps = cth_demo_script;
      g_demo_len = (int)(sizeof(cth_demo_script) / sizeof(cth_demo_script[0]));
      g_demo_prefix = "ge_cth";
   }
   g_demo_on = 0;
   g_demo_phase = 0;
   fe_evt("ui_demo_start script=%s", which == 7 ? "rewind_length" : which == 6 ? "turbo" : which == 5 ? "quit" :
          which == 4 ? "delete" : "menu_button");
}

int ui_demo_running(void)
{
   return g_demo_on >= 0;
}

static unsigned demo_pad(void)
{
   const demo_step *s;
   if (g_demo_on < 0)
      return 0;
   if (g_demo_on >= g_demo_len)
   {
      g_demo_on = -1;
      fe_evt("ui_demo_done");
      return 0;
   }
   s = &g_demo_steps[g_demo_on];
   if (s->pad == DEMO_DUMP)
   {
      /* Taken AFTER this frame is drawn (demo_dump_flush), so the dump is
       * exactly the frame the script reached -- with triple buffering the
       * buffer about to be drawn into still holds the frame from three
       * presents ago, which matters once a screen animates (the capture
       * chip's pulse, the menu's opening). */
      g_demo_dump_pending = 1;
      g_demo_on++;
      g_demo_phase = 0;
      return 0;
   }
   if (g_demo_phase == 0)
      g_demo_phase = s->hold;
   if (g_demo_phase > 0)
   {
      g_demo_phase--;
      if (g_demo_phase == 0)
         g_demo_phase = -(int)s->gap - 1;
      return s->pad;
   }
   g_demo_phase++;
   if (g_demo_phase == 0)
      g_demo_on++;
   return 0;
}

/* The deferred DEMO_DUMP: call once the frame is drawn, before the swap. */
static void demo_dump_named(const char *name)
{
   extern char g_dir_base[];
   char gp[176];
   snprintf(gp, sizeof(gp), "%s/log/%s.bmp", g_dir_base, name);
   if (vid_dump_ge(gp) == 0)
      fe_evt("ge_dump file=%s.bmp ui=1", name);
}

static int ctl_cap_timer(void);         /* the Controls capture clock */

static void demo_dump_flush(void)
{
   static int dump_n;
   char nm[48];
   if (!g_demo_dump_pending)
      return;
   g_demo_dump_pending = 0;
   snprintf(nm, sizeof(nm), "%s_%d", g_demo_prefix, dump_n++);
   demo_dump_named(nm);
   if (ctl_cap_timer() >= 0)
      fe_evt("ge_dump_capture timer=%d", ctl_cap_timer());
}

/* ----- helpers ------------------------------------------------------------ */

const char *ui_group(void)
{
   return g_join_group;
}

/* ADR-0071: relaunch only when the profile actually DIFFERS from its value
 * at settings entry — "touched an even number of times" is not a change.
 * (Also what keeps the harness ui_demo from relaunching the EBOOT.) */
static int g_profile_at_open;

/* ---- WAKE-FROM-SLEEP OVERLAY ------------------------------------------
 *
 * Drawn over the frame the console was showing when it went to sleep, which
 * the suspend path snapshots before it hands the volatile partition back.
 *
 * The frozen frame is the whole point: it says "your game is exactly where you
 * left it" before the player has read a single word.  So it is drawn at full
 * size and full fidelity, then dimmed -- and the plate is deliberately small,
 * because the more of the frame it hides the less it makes that promise.
 *
 * Modal and self-contained: it owns the pad and the swap chain until it
 * returns.  Nothing emulates while it is up, which is correct -- the link is
 * down, the frame is a still, and there is nothing to keep in real time.
 *
 * Returns 0 to resume, 1 to leave for the game list. */

static void footer(const char *hint);   /* defined with the other chrome */
static void ctl_page_reset(void);       /* Settings > Controls state */
#ifdef GPSP_CHEATS
static void cht_page_reset(void);       /* the Cheats page, below */
#endif
static void ov_slots_scan(void);        /* the overlay, below */
static void ov_slots_stat(void);
/* main_psp.c: the power switch waits while the page reads the stick
 * outside the loop's park point (see power_cb there). */
void pwr_hold(const char *why);
void pwr_release(void);
static void ov_shot_probe(void);
static void ov_title_take(void);
extern volatile int g_running;          /* main_psp exit flag (HOME) */

/* Harness `wake_shot = N`: the wake overlay GE-dumps its third frame to
 * log/ge_wake.bmp and continues by itself (docs/UI-OVERLAY.md: the overlay
 * shares the menu's compositing path, so both are checked together). */
static int g_wake_shot;
void ui_wake_shot_arm(void) { g_wake_shot = 1; }

/* Harness `sleep_sim_choice` (main_psp.c sleep simulator): the overlay's
 * answer, 0 Continue / 1 Quit to game list, pressed on its 30th frame as the
 * player would (selection, then X); -1 = the real pad only. */
#ifdef GPSP_PERF_RIG
static int g_wake_inject = -1, g_wake_inject_frame = 30;
void ui_wake_inject(int choice) { g_wake_inject = choice; }
/* `sleep_sim_choice_frame`: the overlay frame the answer lands on (30). */
void ui_wake_inject_frame(int frame) { g_wake_inject_frame = frame > 0 ? frame : 30; }
#else
void ui_wake_inject(int choice) { (void)choice; }
void ui_wake_inject_frame(int frame) { (void)frame; }
#endif

int ui_wake_menu(const uint16_t *frame, int frame_w, int frame_h,
                 const char *game)
{
   int shot_frames = 0, inj_frames = 0;
   (void)inj_frames;
   static const char *lbl[2] = { "Continue", "Quit to game list" };
   const int LX = 20;                 /* wordmark + title margin */
   int sel = 0, first = 1;
   unsigned prev = 0;
   char title[96];

   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;

   /* The filename is not the game's name.  Drop the extension; the region
    * tag stays because two dumps of the same game differ by exactly that. */
   {
      const char *dot;
      snprintf(title, sizeof(title), "%s", game ? game : "");
      dot = strrchr(title, '.');
      if (dot && (dot - title) > 0)
         title[dot - title] = 0;
   }

   /* MUST loop on g_running, not for(;;).
    *
    * HOME is the first thing a lot of people will press on a console they
    * picked up days later, and it fires the exit callback, which sets
    * g_running = 0 and then WAITS for the app to leave.  A modal loop that
    * never looks at the flag leaves the kernel at "Please wait..." forever,
    * with a hard reset as the only way out.  Every other modal screen in this
    * file already loops on g_running; this one did not.
    *
    * Falling out returns 0 (resume), which is the harmless answer: the main
    * loop's own `while (g_running)` is false by then, so it goes straight to
    * the normal shutdown -- SRAM flushed, session torn down, ExitGame. */
   while (g_running)
   {
      SceCtrlData pad;
      unsigned now, edges;
      int i;

      sceCtrlPeekBufferPositive(&pad, 1);
      now   = pad.Buttons;
      edges = now & ~prev;
      prev  = now;
      /* Swallow whatever was held as the console woke: the player slid a
       * switch, they have not chosen anything yet. */
      if (first)
         { edges = 0; first = 0; }
#ifdef GPSP_PERF_RIG
      if (g_wake_inject >= 0 && ++inj_frames == g_wake_inject_frame)
      {
         sel = g_wake_inject ? 1 : 0;
         edges = PSP_CTRL_CROSS;
      }
#endif

      if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN))
         sel ^= 1;
      if (edges & PSP_CTRL_CROSS)
         return sel;
      if (edges & PSP_CTRL_CIRCLE)
         return 0;              /* O is always "back to what I was doing" */

      vid_overlay_begin(1);

      /* 1. THE GAME, AS IT WAS.  This is the whole idea -- it says "you are
       *    exactly where you left off" before a word has been read. */
      if (frame)
         vid_image_screen(frame, 256, 256, frame_w, frame_h, 255);
      /* 2. Hold it back far enough for type to read over any scene, bright
       *    or dark.  No panel: the picture IS the backdrop, and a plate over
       *    it only hides the thing worth showing. */
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, C_BG_BOT, frame ? 170 : 240);
      /*    ...then a ramp climbing out of the bottom edge, so the choices and
       *    the footer sit on something solid while the top of the frame stays
       *    as visible as the scrim allows.  One strip, interpolated per pixel
       *    -- a stack of rects bands badly at 565 (see vid_gradient_a). */
      vid_gradient_a(0, VID_SCR_H - 120, VID_SCR_W, 120, C_BG_BOT, 0, 165);

      /* 3. Identity, hard into the top-left corner.  y 17: the 151x24
       *    wordmark keeps the 130x22 one's centre row (29). */
      vid_logo(LX, 17, C_TITLE, 255);
      if (title[0])
         vid_text(LX + 2, 48, title, C_DIM);

      /* 4. The choices, same left margin, same selection language as the
       *    browser: an accent edge bar and a soft fill, never a slab. */
      /*    Flush to the screen's left edge: the selection bleeds off it
       *    rather than floating, which is what stops this reading as a dialog
       *    box without one being drawn. */
      for (i = 0; i < 2; i++)
      {
         int ry = 168 + i * 32;
         int w  = vid_text_w(lbl[i]) + 40;
         if (i == sel)
         {
            /* Cap height centred (vid_band_y), like every overlay band. */
            int by = vid_band_y(ry, FE_FONT_H + 12);
            vid_rect(0, by, w, FE_FONT_H + 12, C_ACCENT, 40);
            vid_rect(0, by, 4, FE_FONT_H + 12, C_ACCENT, 255);
         }
         vid_text(18, ry, lbl[i], i == sel ? C_SEL : C_ITEM);
      }

      /* 5. Hints on the screen's own bottom edge, in the same footer the
       *    rest of the UI uses -- not floating under a box. */
      footer("X select     O resume     DPAD change");

      vid_overlay_end();
      if (g_wake_shot && ++shot_frames == 3)
      {
         g_wake_shot = 0;
         demo_dump_named("ge_wake");
         return 0;
      }
      sceDisplayWaitVblankStart();
      vid_swap();
   }
   return 0;      /* HOME: let the main loop run its clean shutdown */
}

void ui_open(void)
{
   ctl_page_reset();
   g_active = 1;
   g_screen = SCR_MENU;
   g_cursor = 0;
   g_ov_open_k = 0;              /* the opening transition (screen_menu) */
   ov_title_take();
   g_prev_pad = 0xFFFFFFFFu;   /* swallow the opening chord */
   {
      /* "n of 5 used" beside Save states: five stats, once, with the core
       * paused -- before the opening transition, never inside it. */
      unsigned t0 = sceKernelGetSystemTimeLow();
      ov_slots_stat();
      FE_EVT_ONLY(t0);
      fe_evt("ui_open slots_used=%d stat_us=%u", g_ov_used,
             (unsigned)(sceKernelGetSystemTimeLow() - t0));
   }
}

void ui_close(void)
{
   /* Back to the game, maybe after a state load: an ambient snapshot (no
    * art, "art, else game") is retaken from what is on screen now. */
   vid_ambient_resnap();
   if (g_settings_dirty)
   {
      pcfg_save();
      g_settings_dirty = 0;
   }
   g_active = 0;
   ctl_page_reset();
   if (g_demo_on >= 0)
   {
      /* The demo's final Resume closes the UI mid-script by design. */
      g_demo_on = -1;
      fe_evt("ui_demo_done");
   }
   fe_evt("ui_close");
}

int ui_active(void)
{
   return g_active;
}

int ui_home_press(void)
{
   if (!g_active || g_screen != SCR_MENU || ui_capturing() ||
       ui_demo_running())
      return 0;
   fe_evt("ui_home_close");
   ui_close();
   return 1;
}

static void screen_to(int scr)
{
   static const char *names[] __attribute__((unused)) =
      { "menu", "settings", "wireless", "scan", "mystery_gift", "state_slots",
        "controls", "host", "join_ask", "cheats" };
   if (g_screen != scr && g_settings_dirty)
   {
      pcfg_save();
      g_settings_dirty = 0;
   }
   g_screen = scr;
   g_cursor = 0;
   ctl_page_reset();             /* no capture survives a screen change */
   g_slot_ask = 0;               /* nor a delete/overwrite question */
   g_slot_note[0] = '\0';
   if (scr == SCR_SETTINGS)
   {
      g_profile_at_open = g_pcfg.me_mode;
      g_set_scroll = 0;
   }
   if (scr == SCR_STATE_SLOTS)
   {
      ov_slots_scan();
      ov_shot_probe();
   }
#ifdef GPSP_CHEATS
   if (scr == SCR_CHEATS)
      cht_page_reset();
#endif
   fe_evt("ui_screen name=%s", names[scr]);
}

/* Edge/repeat filter: returns buttons to act on this frame. */
static unsigned pad_edges(unsigned pad)
{
   unsigned edges = pad & ~g_prev_pad;
   unsigned dirs = pad & (PSP_CTRL_UP | PSP_CTRL_DOWN |
                          PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
   if (dirs && dirs == (g_prev_pad & dirs))
   {
      if (++g_rep_timer >= 18)
      {
         g_rep_timer = 13;
         edges |= dirs;
      }
   }
   else
      g_rep_timer = 0;
   g_prev_pad = pad;
   return edges;
}

/* ----- themed page chrome ------------------------------------------------- */

#define HDR_H  30
#define FTR_H  20

/* Full-page background + header bar.  `right` (optional) is right-aligned
 * in the header — room code, game count, session state. */
static void page_header(const char *title, const char *right);

static void page(const char *title, const char *right)
{
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;
   if (g_theme_black)
      /* HARNESS: plain black page — the at-a-glance differentiator from the
       * playable build. */
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, 0x0000, 255);
   else
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, C_BG_TOP, 255);
   page_header(title, right);
}

/* The header bar alone, for a page whose background is a picture. */
static void page_header(const char *title, const char *right)
{
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;
   vid_rect(0, 0, VID_SCR_W, HDR_H, C_HDR_TOP, 255);
   vid_rect(0, HDR_H, VID_SCR_W, 1, C_ACCENT, 255);
   vid_text_hd(14, (HDR_H - FE_FONT_H) / 2 - 2, title, C_TITLE);
   if (right && right[0])
      vid_text(VID_SCR_W - 14 - vid_text_w(right),
               (HDR_H - FE_FONT_H) / 2, right, C_VALUE);
}

static void footer(const char *hint)
{
   vid_rect(0, VID_SCR_H - FTR_H, VID_SCR_W, 1, C_ACCENT_DK, 180);
   vid_rect(0, VID_SCR_H - FTR_H + 1, VID_SCR_W, FTR_H - 1, C_HDR_BOT, 220);
   vid_text_center(VID_SCR_H - FTR_H + 2, hint, C_DIM);
}

/* ===== THE OVERLAY: Fable's Direction A (docs/UI-OVERLAY.md) ==============
 *
 * The wake-from-sleep screen's language, extended to every in-game screen:
 * the game frame stays up at full fidelity behind a scrim, the wordmark sits
 * hard in the top-left, the selection bleeds off the left edge, and a ramp
 * out of the bottom edge gives the footer something solid.  No panel is ever
 * drawn; the picture is the backdrop.
 *
 * The numbers are the design's (builds/menu-mockups/mockups.py, class
 * Overlay), drawn through the same primitives the design model renders with,
 * so a GE dump can be compared with the model pixel for pixel
 * (tools/ui_model/).
 *
 * MEMORY.  Nothing here allocates.  The frame is the wake snapshot
 * (main_psp.c, the netdrv arena's tail) taken at menu_open(); the slot
 * thumbnails are five 64x64 textures from the same tail.  Either may be
 * NULL -- then the screen falls back to a flat page / empty plates. */
static void clip_title(char *t, int px);    /* with the browser, below    */

void ui_set_backdrop(const uint16_t *frame, int w, int h)
{
   g_ov_frame = frame;
   g_ov_fw = w > 0 ? w : 240;
   g_ov_fh = h > 0 ? h : 160;
}

void ui_set_thumb_buffer(uint16_t *buf)
{
   g_ov_thumbs = buf;
}

/* Harness only (reached through the ini a release build cannot read): a raw
 * 480x272 PSP-5650 picture, drawn 1:1 behind the menu in place of the game,
 * so the design model can be given the very same backdrop. */
void ui_backdrop_inject(const char *rel)
{
   extern char g_dir_base[];
   char path[176];
   SceUID fd;
   int y, ok = 1;
   snprintf(path, sizeof(path), "%s/%s", g_dir_base, rel);
   fd = sceIoOpen(path, PSP_O_RDONLY, 0);
   if (fd < 0)
   {
      fe_evt("ui_backdrop file=%s MISSING", rel);
      return;
   }
   g_ov_full = (uint16_t *)memalign(64, 512 * 512 * 2);
   if (!g_ov_full)
   {
      sceIoClose(fd);
      fe_evt("ui_backdrop file=%s NO_MEMORY", rel);
      return;
   }
   memset(g_ov_full, 0, 512 * 512 * 2);
   for (y = 0; y < VID_SCR_H && ok; y++)
      ok = sceIoRead(fd, g_ov_full + y * 512, VID_SCR_W * 2) ==
           VID_SCR_W * 2;
   sceIoClose(fd);
   sceKernelDcacheWritebackRange(g_ov_full, 512 * 512 * 2);
   fe_evt("ui_backdrop file=%s ok=%d", rel, ok);
}

static void ov_theme(void)
{
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;
}

/* The game's name from the ROM it was booted from (the state base is the
 * ROM path with its extension replaced), directories and extensions off.
 * The region tag stays: two dumps of one game differ by exactly that. */
static void ov_title_take(void)
{
   const char *src = g_state_base[0] ? g_state_base : g_pcfg.last_rom;
   const char *b = strrchr(src, '/');
   char *dot;
   int pass;
   size_t n;
   src = b ? b + 1 : src;
   n = strlen(src);
   if (n >= sizeof(g_ov_title))        /* clip_title trims it to fit anyway */
      n = sizeof(g_ov_title) - 1;
   memcpy(g_ov_title, src, n);
   g_ov_title[n] = 0;
   /* "Game.st0", "Game.gb.st0", "Game.gba": up to two extensions. */
   for (pass = 0; pass < 2; pass++)
   {
      dot = strrchr(g_ov_title, '.');
      if (!dot || dot == g_ov_title)
         break;
      if (pass == 1 && strcasecmp(dot, ".gb") && strcasecmp(dot, ".gbc") &&
          strcasecmp(dot, ".gba"))
         break;
      *dot = '\0';
   }
   clip_title(g_ov_title, 436);
}

/* The picture, held back by a scrim, then the ramp out of the bottom edge
 * (the wake overlay's own three layers). */
static void ov_backdrop(int scrim)
{
   ov_theme();
   if (g_ov_browse)
   {
      /* Settings from the ROM browser: no game frame exists, so the
       * selected game's ambient bake stands in for it -- the loading
       * screen's "L2" background (browser_settings baked it once).  No
       * art: the console's palette ramp, which is a page already and so
       * takes no scrim (the loading screen draws text straight onto it). */
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H,
               g_theme_black ? 0x0000 : C_BG_TOP, 255);
      if (!vid_ambient_image(0, 0, VID_SCR_W, VID_SCR_H, 255))
      {
         vid_gradient_a(0, 0, VID_SCR_W, VID_SCR_H,
                        skin_of(g_pcfg.console)->pal[0],
                        g_pcfg.theme == 1 ? LOAD_PAL_RAMP_LIGHT
                                          : LOAD_PAL_RAMP_DARK, 0);
         scrim = 0;
      }
   }
   else if (g_ov_full)
      vid_image_px(0, 0, VID_SCR_W, VID_SCR_H, g_ov_full, 512, 512);
   else if (g_ov_frame)
   {
      vid_image_screen(g_ov_frame, 256, 256, g_ov_fw, g_ov_fh, 255);
   }
   else
   {
      /* Nothing to show (no snapshot memory): the flat page. */
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H,
               g_theme_black ? 0x0000 : C_BG_BOT, 255);
      scrim = 0;
   }
   if (scrim > 0)
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, C_BG_BOT, scrim);
   vid_gradient_a(0, OV_RAMP_Y, VID_SCR_W, VID_SCR_H - OV_RAMP_Y, C_BG_BOT,
                  0, 165);
}

/* ---- THE BATTERY, in the overlay header (GitHub issue #8) ----------------
 *
 * With Menu = HOME the system's HOME popup -- the only place a player could
 * read the battery -- belongs to the in-game menu, so the menu shows it:
 * a small cell and the percentage on the header's top line, where the room
 * used to be; the room moves to the line under it.  The console badge stays
 * exactly where 3.5.0 drew it.
 *
 * Read with the firmware's own calls, at most once every five seconds and
 * only from here -- that is, only while an overlay screen is up and nothing
 * is being emulated.  No battery, or a call that fails (a console on mains
 * with the pack out; the first moments after a wake), and nothing is drawn:
 * the header is then the one 3.5.0 drew, room and all.  A wrong number is
 * worse than none. */
static struct { unsigned t_us; int have, pct, plugged; } g_batt;
#ifdef GPSP_PERF_RIG
/* Harness `ui_batt`: -1 real (default), -2 none, else percent + 1000 if on
 * power -- PPSSPP always reports a full battery. */
static int g_batt_fake = -1;
void ui_batt_inject(int v) { g_batt_fake = v; g_batt.t_us = 0; }
#else
void ui_batt_inject(int v) { (void)v; }
#endif

static void batt_poll(void)
{
   unsigned now = (unsigned)sceKernelGetSystemTimeLow();
   int pct;
   if (g_batt.t_us && now - g_batt.t_us < 5000000u)
      return;
   g_batt.t_us = now ? now : 1;
#ifdef GPSP_PERF_RIG
   if (g_batt_fake != -1)
   {
      g_batt.have = g_batt_fake >= 0;
      g_batt.plugged = g_batt_fake >= 1000;
      g_batt.pct = g_batt_fake % 1000;
      if (g_batt.pct > 100)
         g_batt.have = 0;
      return;
   }
#endif
   pct = scePowerGetBatteryLifePercent();
   g_batt.have = scePowerIsBatteryExist() > 0 && pct >= 0 && pct <= 100;
   g_batt.pct = pct;
   g_batt.plugged = scePowerIsPowerOnline() > 0 ||
                    scePowerIsBatteryCharging() > 0;
}

#define BATT_LOW      15                /* percent: the cell and number warn */
#define BATT_W        16
#define BATT_H        10
#define BATT_GAP      4                 /* cell to number */
#define BATT_ROOM_Y   44                /* the room on line 2: clear of the
                                         * save-state preview's top edge (62) */
#define BATT_BOLT_W   7                 /* bolt + its gap, left of the cell */

/* Right-aligned at xr on the header row y; nothing left of x_min (the
 * badge's edge plus a gap: the bolt is dropped before it would touch).
 * Returns 1 if the battery was drawn. */
static int ov_battery(int xr, int y, int x_min)
{
   static const signed char bolt[7][2] = {
      {3,2}, {2,2}, {1,2}, {0,5}, {2,2}, {1,2}, {0,2}
   };
   char txt[8];
   int tw, x0, yt, fw, low, i;
   uint16_t col, fill;
   batt_poll();
   if (!g_batt.have)
      return 0;
   low = g_batt.pct <= BATT_LOW && !g_batt.plugged;
   /* The themes' own warn colour is white / black by design ("the words
    * carry the alarm"); a low battery has no words, so it gets a red. */
   col = !low ? C_VALUE : g_pcfg.theme == 1 ? RGB565(0xC0, 0x2A, 0x2A)
                                            : RGB565(0xF0, 0x60, 0x60);
   fill = g_batt.plugged ? (g_pcfg.theme == 1 ? RGB565(0x1E, 0x8A, 0x4A)
                                              : RGB565(0x78, 0xD6, 0x8C))
                         : col;
   snprintf(txt, sizeof(txt), "%d%%", g_batt.pct);
   tw = vid_text_w(txt);
   x0 = xr - tw - BATT_GAP - BATT_W;
   if (x0 < x_min)
      return 0;                         /* no room: leave the header alone */
   vid_text(xr - tw, y + 2, txt, col);
   yt = y + 2 + (FE_FONT_H - BATT_H) / 2 + 1;
   /* the cell: a 1 px outline, the terminal, the charge */
   vid_rect(x0, yt, BATT_W - 2, 1, col, 255);
   vid_rect(x0, yt + BATT_H - 1, BATT_W - 2, 1, col, 255);
   vid_rect(x0, yt + 1, 1, BATT_H - 2, col, 255);
   vid_rect(x0 + BATT_W - 3, yt + 1, 1, BATT_H - 2, col, 255);
   vid_rect(x0 + BATT_W - 2, yt + 3, 2, BATT_H - 6, col, 255);
   fw = ((BATT_W - 6) * g_batt.pct + 50) / 100;
   if (fw < 1)
      fw = 1;
   vid_rect(x0 + 2, yt + 2, fw, BATT_H - 4, fill, 255);
   if (g_batt.plugged && x0 - BATT_BOLT_W >= x_min)
      for (i = 0; i < 7; i++)
         vid_rect(x0 - BATT_BOLT_W + bolt[i][0], yt + 1 + i, bolt[i][1], 1,
                  fill, 255);
   return 1;
}

/* The menu header's right-hand cluster: console badge, then the battery;
 * under it the room ("Room 7", room_label.h) -- or LINKED during a session.
 * The ONLY place the menus name the room in their header (3.1 also printed
 * the raw group id on a line under it in Settings and Wireless: the same
 * text twice).  Without a battery reading the room is on the top line, as
 * in 3.5.0. */
static int g_ov_room2_w;   /* > 0: the room is on line 2 and this wide */
/* With the battery in the room's old place the room is only shown where it
 * means something: on the wireless screens (and LINKING), and everywhere
 * while a session is up (LINKED / HOSTING).  The menu, save states,
 * settings and controls no longer carry it.  Without a battery reading the
 * header is 3.5.0's and the room is always there. */
static int g_ov_room_show = 1;
static int ov_cluster_text(const char *room);
static void ov_cluster(void)
{
   char lbl[ROOM_LABEL_CAP];
   if (g_link.state == UI_LINK_HOSTING || g_ov_linked)
      g_ov_room_show = 1;
   /* A cable host waiting for the other PSP reads HOSTING (docs/
    * GBA-LINK-UI.md); any other session, LINKED. */
   ov_cluster_text(g_link.state == UI_LINK_HOSTING ? "HOSTING" :
                   g_ov_linked ? "LINKED"
                               : room_label(g_pcfg.group, lbl, sizeof(lbl)));
}

/* The cluster with its text given; returns the badge's left edge (where the
 * LINKING screen's busy ring hangs). */
static int ov_cluster_text(const char *room)
{
   const ui_skin *s = skin_of(g_pcfg.console);
   const int y = 19;
   int x = 460, bw, k, room_w = vid_text_w(room), room_x;
   /* The badge is placed from the room's width whether or not the room is
    * drawn beside it: it must not move when the battery takes that slot. */
   x -= room_w;
   room_x = x;
   x -= 12;
   bw = 8 + vid_text_w(s->id) + 8;
   x -= bw;
   if (ov_battery(460, y, x + bw + 4))
   {
      if (g_ov_room_show)
         vid_text(460 - room_w, BATT_ROOM_Y, room, C_DIM);
      g_ov_room2_w = g_ov_room_show ? room_w : 0;
   }
   else
   {
      vid_text(room_x, y + 2, room, C_DIM);
      g_ov_room2_w = 0;
   }
   vid_rect(x, y, bw, 20, C_CON, g_pcfg.theme == 1 ? 36 : 48);
   for (k = 0; k < s->pal_n; k++)
      vid_rect(x + (k * bw) / s->pal_n, y + 18,
               ((k + 1) * bw) / s->pal_n - (k * bw) / s->pal_n, 2,
               s->pal[k], 255);
   vid_text(x + 8, y + 2, s->id, C_CON);
   return x;
}

/* The game's name on line 2, clipped short of the room when the room is on
 * that line too (after ov_cluster / ov_cluster_text). */
static void ov_sub(const char *sub)
{
   char t[100];
   if (!sub || !sub[0])
      return;
   snprintf(t, sizeof(t), "%s", sub);
   clip_title(t, g_ov_room2_w ? 460 - g_ov_room2_w - 14 - 22 : 436);
   vid_text(22, 48, t, C_DIM);
}

/* Wordmark hard top-left; an hd screen title after it; the game's name
 * under it; the cluster top-right. */
static void ov_head(const char *sub, const char *title)
{
   /* y 17: the 151x24 wordmark centred where the 130x22 one was (row 29,
    * the cluster's centre); the title follows its width. */
   vid_logo(20, 17, C_TITLE, 255);
   if (title)
      vid_text_hd(20 + VID_LOGO_W + 14, 16, title, C_SEL);
   g_ov_room_show = title && (!strcmp(title, "WIRELESS") ||
                              !strcmp(title, "HOST") ||
                              !strcmp(title, "JOIN") ||
                              !strcmp(title, "MYSTERY GIFT"));
   ov_cluster();               /* first: line 2 is shared with the room */
   ov_sub(sub);
}

static void ov_text_right(int xr, int y, const char *s, uint16_t c);

/* A screen's own status word on line 2 ("SCANNING", "LISTENING"): right-
 * aligned at the edge, or left of the room when the room is on that line. */
static void ov_status_right(const char *s)
{
   if (g_ov_room2_w)
      ov_text_right(460 - g_ov_room2_w - 14, BATT_ROOM_Y, s, C_DIM);
   else
      ov_text_right(460, 48, s, C_DIM);
}

/* The busy ring left of the console badge (the loading screen's, on the
 * Overlay's header row: centre row 29). */
static void ov_ring(int badge_x)
{
   static const signed char ring[8][2] = {
      {0,-7}, {5,-5}, {7,0}, {5,5}, {0,7}, {-5,5}, {-7,0}, {-5,-5}
   };
   unsigned phase = ((unsigned)sceKernelGetSystemTimeLow() / 100000u) % 8u;
   int i, cx = badge_x - HDR_RING_DX, cy = 29;
   for (i = 0; i < 8; i++)
      vid_rect(cx + ring[i][0] - 1, cy + ring[i][1] - 1, 3, 3, C_TITLE,
               50 + 25 * (int)(((unsigned)i + 8u - phase) % 8u));
}

/* THE LINKING SCREEN (ui_psp.h; docs/GBA-LINK-UI.md): one screen for every
 * mode and both consoles, title LINKING, the busy ring left of the console
 * badge as on the loading screen.  Line 1 says what is happening, line 2
 * the step (its amount on the right), the 3 px bar in the console colour
 * is the loading screen's bar.  A failure replaces the two lines and the
 * footer becomes O back. */
void ui_linking_draw(const ui_linking_view *v)
{
   char sub[100], lbl[ROOM_LABEL_CAP];
   int bx;
   if (!v)
      return;
   vid_overlay_begin(1);
   g_ov_browse = 0;
   ov_backdrop(OV_SCRIM);
   vid_logo(20, 17, C_TITLE, 255);
   vid_text_hd(20 + VID_LOGO_W + 14, 16, "LINKING", C_SEL);
   snprintf(sub, sizeof(sub), "%s", v->game ? v->game : g_ov_title);
   g_ov_room_show = 1;         /* LINKING names the room */
   bx = ov_cluster_text(v->room && v->room[0]
                           ? v->room
                           : room_label(g_pcfg.group, lbl, sizeof(lbl)));
   ov_sub(sub);
   if (v->busy && !v->fail)
      ov_ring(bx);
   if (v->fail)
   {
      if (v->l1)
         vid_text(22, 72, v->l1, C_VALUE);
      if (v->l2)
         vid_text(22, 72 + FE_FONT_H + 2, v->l2, C_DIM);
      footer(v->footer ? v->footer : VID_GLYPH_O " back");
   }
   else
   {
      if (v->l1)
         vid_text(22, 72, v->l1, C_ITEM);
      if (v->l2)
         vid_text(22, 72 + FE_FONT_H + 2, v->l2, C_DIM);
      if (v->amount)
         ov_text_right(460, 72 + FE_FONT_H + 2, v->amount, C_DIM);
      if (v->frac >= 0)
      {
         int f = v->frac > 1000 ? 1000 : v->frac;
         vid_rect(20, 116, 440, 3, C_ACCENT_DK, 160);
         if (f > 0)
            vid_rect(20, 116, 440 * f / 1000, 3, C_CON, 255);
      }
      if (v->l3)
         vid_text(22, 130, v->l3, C_DIM);
      footer(v->footer ? v->footer : VID_GLYPH_O " cancel");
   }
   vid_overlay_end();
}

/* The selection: a soft fill and a 4 px edge, flush to the screen's left
 * edge so it bleeds off it rather than floating (the wake overlay's).
 * `y` is the ROW'S TEXT y: the band is placed so the text's cap height sits
 * in its optical centre (vid_band_y; docs/UI-OVERLAY.md §10).  The design's
 * `y - 4` centred the 16 px line box instead, which left the letters 1.5 px
 * low in a 24 px band and, in Settings' 20 px one, resting on its floor. */
static void ov_band(int y, int w, int h)
{
   int by = vid_band_y(y, h);
   vid_rect(0, by, w, h, C_ACCENT, 40);
   vid_rect(0, by, 4, h, C_ACCENT, 255);
}

static void ov_text_right(int xr, int y, const char *s, uint16_t c)
{
   vid_text(xr - vid_text_w(s), y, s, c);
}

/* The footer's stand-in when the page has something to SAY (capture,
 * conflict, a refusal): one plate across the bottom edge. */
static void ov_plate(const char *msg)
{
   vid_rect(0, OV_PLATE_Y, VID_SCR_W, FTR_H, C_ACCENT, 40);
   vid_text_center(OV_PLATE_Y + 2, msg, C_SEL);
}

/* ---- chips: the Controls page's one new idiom ---------------------------
 * A plate CHIP_H tall, text inset 6 px, in four states:
 *   bound   C_ACCENT at alpha 40 (28 light), C_SEL text
 *   locked  C_CARD, C_DIM text (a fixed chord is ONE chip: START+SELECT)
 *   blank   1 px C_DIM outline at alpha 160 around the word "none"
 *   capture 1 px C_CON outline around "press...", alpha-pulsed */
enum { CHIP_BOUND, CHIP_LOCKED, CHIP_BLANK, CHIP_CAPTURE };
#define CHIP_H 16

static int chip_w(const char *label)
{
   return 12 + vid_text_w(label);
}

static void ov_outline(int x, int y, int w, int h, uint16_t c, int a)
{
   vid_rect(x, y, w, 1, c, a);
   vid_rect(x, y + h - 1, w, 1, c, a);
   vid_rect(x, y, 1, h, c, a);
   vid_rect(x + w - 1, y, 1, h, c, a);
}

/* `y` is the row's TEXT y, as for every other row element; the plate is
 * placed around it with the cap height centred (vid_band_y). */
static int chip(int x, int y, const char *label, int kind, int alpha)
{
   int w = chip_w(label);
   int ty = y;
   y = vid_band_y(ty, CHIP_H);
   switch (kind)
   {
   case CHIP_BOUND:
      vid_rect(x, y, w, CHIP_H, C_ACCENT, g_pcfg.theme == 1 ? 28 : 40);
      vid_text(x + 6, ty, label, C_SEL);
      break;
   case CHIP_LOCKED:
      vid_rect(x, y, w, CHIP_H, C_CARD, 255);
      vid_text(x + 6, ty, label, C_DIM);
      break;
   case CHIP_BLANK:
      ov_outline(x, y, w, CHIP_H, C_DIM, 160);
      vid_text(x + 6, ty, label, C_DIM);
      break;
   default:
      ov_outline(x, y, w, CHIP_H, C_CON, alpha);
      vid_text(x + 6, ty, label, C_CON);
      break;
   }
   return w;
}

/* Chips joined by '+' in C_DIM; an empty chord is the "none" outline. */
static int combo_w(const char *const *parts, int n)
{
   int i, w = 0;
   if (!n)
      return chip_w("none");
   for (i = 0; i < n; i++)
      w += chip_w(parts[i]);
   return w + (n - 1) * (vid_text_w("+") + 8);
}

static int combo_right(int xr, int y, const char *const *parts, int n,
                       int kind, int alpha)
{
   int w = combo_w(parts, n), x = xr - w, i;
   if (!n)
   {
      chip(x, y, "none", CHIP_BLANK, 160);
      return w;
   }
   for (i = 0; i < n; i++)
   {
      if (i)
      {
         vid_text(x + 4, y, "+", C_DIM);
         x += vid_text_w("+") + 8;
      }
      x += chip(x, y, parts[i], kind, alpha);
   }
   return w;
}

/* A 2 px scrollbar: C_CARD track, C_ACCENT_DK thumb (the design's
 * scrollbar(): top h*pos/total, length max(8, h*view/total)). */
static void ov_scrollbar(int x, int y, int h, int pos, int view, int total)
{
   int ty, th;
   if (total <= view || total <= 0)
      return;
   ty = h * pos / total;
   th = h * view / total;
   if (th < 8)
      th = 8;
   vid_rect(x, y, 2, h, C_CARD, 255);
   vid_rect(x, y + ty, 2, th, C_ACCENT_DK, 255);
}

/* Button words for the menu, in ctl_button_order() order: the face buttons
 * as the baked glyphs (tools/bake_font.py EXTRAS), the rest spelled the way
 * the rest of the UI spells them.  CONFIG.INI uses ctl_map.c's own words. */
static const char *const CTL_UI_NAMES[CTL_NBUTTONS] = {
   "SELECT", "START", "L", "R", "UP", "DOWN", "LEFT", "RIGHT",
   VID_GLYPH_TRI, VID_GLYPH_O, VID_GLYPH_X, VID_GLYPH_SQ,
   "STICK UP", "STICK DOWN", "STICK LEFT", "STICK RIGHT"
};

static ui_action screen_controls(unsigned edges, unsigned pad);
static ui_action screen_state_slots(unsigned edges);
#ifdef GPSP_CHEATS
static void cht_page_reset(void);
#endif

/* ----- settings screen ---------------------------------------------------- */

/* Sectioned.  Trading profile stays FIRST (ADR-0071: it is the one setting
 * that decides whether a trade completes, and the only one that costs a
 * restart).  Header rows are labels, not stops — the cursor skips them. */
/* SET_PROFILE (Media Engine mode) was removed 2026-09-08: the single-core
 * pipeline is deprecated.  It has nowhere near the headroom to be useful --
 * the ME renderer is what makes full speed possible -- and offering it as a
 * choice only let someone end up in the slow path by accident.
 *
 * THE CODE PATH STAYS.  me_rend_teardown() falls back to CPU rendering from
 * six places (watchdog, input wedge, late frame, np_start, exit, failed ME
 * init), it is the reference the me_capture_mode=2 validation oracle compares
 * against, and it is the only renderer that exists under PPSSPP, which has no
 * Media Engine.  `me_mode` also survives as a config.ini key so a bug report
 * can still be bisected without a special build.  Only the menu row is gone. */
/* DISPLAY is PER CONSOLE (docs/DISPLAY-FEATURES.md): its rows edit the
 * profile of the console in context -- the running game's, or in the
 * browser the console TRIANGLE last switched to -- and its header says
 * which.  Everything else on this screen is global. */
enum { SET_HDR_WL, SET_ROOM, SET_LINKMODE, SET_OSD, SET_SHARE, SET_JOIN,
       SET_FASTLINK,
       SET_HDR_VID, SET_SCALE, SET_FILTER, SET_AMBIENT, SET_GBPAL,
       SET_HDR_UI, SET_THEME, SET_SHELL,
       SET_HDR_GAME, SET_FFMULT, SET_FFMODE, SET_FPS,
#ifdef REWIND
       SET_REWIND, SET_REWIND_MB,
#endif
       SET_HDR_CTL, SET_STICK, SET_CONTROLS,
       SET_COUNT };

static const struct { unsigned char header; const char *label; } set_rows[SET_COUNT] = {
   { 1, "WIRELESS" },
   { 0, "Room" },
   /* Link Cable / Wireless Adapter, PER GAME (docs/GBA-LINK-UI.md): the
    * design puts it here, between Room and Session overlay. */
   { 0, "Link mode" },
   { 0, "Session overlay" },
   /* BROWSER ONLY (docs/SAVE-SHARE.md): hidden in the in-game Settings, so
    * every row there keeps its place and its count (set_row_shown).  Share
    * game data HOSTS a data-share room on the Settings room; the row under
    * it is the ordinary room list -- the in-game Wireless menu's -- which
    * the browser had no way to reach.  There is no other entry: a joined
    * room that turns out to be a data share opens its menu by itself. */
   { 0, "Share game data" },
   { 0, "Join: scan for rooms" },
   /* BROWSER ONLY too, and LAST in the section, so no row a harness walker
    * counts has moved (in game the row is in the Wireless menu itself). */
   { 0, "Fast link" },
   { 1, "DISPLAY" },              /* + " (GBA)" etc., drawn per console */
   { 0, "Video scale" },
   { 0, "Video filter" },
   { 0, "Ambient bars" },
   { 0, "GB palette" },
   { 1, "INTERFACE" },
   { 0, "Theme" },
   { 0, "Menu style" },
   { 1, "GAMEPLAY" },
   { 0, "Fast-forward" },
   { 0, "FF button (Square)" },
   /* "A/B buttons" (3.0's btn_swap) was here.  Button mapping does the same
    * and more, and an old swapped config is converted into bind_a/bind_b at
    * load (ctl_load_ini), so the row went.  Rows are walked by count: the
    * harness walkers only ever cross rows ABOVE this point, or wrap UP to
    * Button mapping, so none of their arithmetic moved. */
   { 0, "FPS counter" },
#ifdef REWIND
   /* Hold the Rewind button (Triangle unless remapped) and the game runs
    * backwards.  Both rows are read when a game is started. */
   { 0, "Rewind" },
   { 0, "Rewind length" },
#endif
   /* Last, so every row above keeps its position (the harness ui_demo walks
    * this list by count).  X opens the page; it is not a value to cycle. */
   { 1, "CONTROLS" },
   /* Above Button mapping, which stays LAST: the harness walkers reach it
    * by wrapping up from the top. */
   { 0, "Thumbstick" },
   { 0, "Button mapping" },
};

/* A GBA profile has no DMG palette: the row stays visible, greyed, and the
 * cursor steps over it. */
static int linkmode_row_enabled(void);
/* Rows that exist only in the browser's Settings: not drawn, not counted
 * and not stopped on anywhere else. */
static int set_row_shown(int idx)
{
   return (idx != SET_SHARE && idx != SET_JOIN && idx != SET_FASTLINK) ||
          g_ov_browse;
}

/* "Fast link" (docs/GBA-LINK-UI.md): two frames of buttons per packet in a
 * GBA Link Cable session.  The HOST's setting is the session's; a guest's is
 * ignored (and left alone).  It does nothing for a GB/GBC link or for the
 * Wireless Adapter, so the row is greyed there. */
#define FASTLINK_HINT1 "For a strong connection: slightly faster link play."
#define FASTLINK_HINT2 "The host's setting is used."
static int fastlink_row_enabled(void)
{
   return pcfg_display_console() == FE_CONSOLE_GBA;
}

static int set_row_enabled(int idx)
{
   if (!set_row_shown(idx))
      return 0;
   if (idx == SET_GBPAL)
      return pcfg_display_console() != FE_CONSOLE_GBA;
   if (idx == SET_LINKMODE)
      return linkmode_row_enabled();
   if (idx == SET_FASTLINK)
      return fastlink_row_enabled();
   return 1;
}

/* The list outgrew the page when "Menu style" landed.  The pitch has
 * already been shaved twice (see below), so instead the page scrolls:
 * rows keep their absolute layout and the whole column is shifted by
 * one offset, which keeps each section header travelling with its
 * items. */
/* The overlay's viewport (docs/UI-OVERLAY.md): under the wordmark and the
 * room code, clipped by the GE scissor, with a 2 px scrollbar at x=470. */
#define SET_VIEW_TOP  76
#define SET_VIEW_BOT  244
/* The cursor row's band (set_item): 20 rows on a 16 px pitch, cap height
 * centred, so it spans the row's text y .. y+19 (vid_band_y). */
#define SET_BAND_W    460
#define SET_BAND_H    20

/* The lowest row the cursor row's band reaches: what the scroll has to keep
 * inside the viewport.  The text box (y + FE_FONT_H) was the old measure,
 * from when the band sat ABOVE the text; now the band is the lower edge. */
static int set_row_bot(int y)
{
   int b = vid_band_y(y, SET_BAND_H) + SET_BAND_H;
   return b > y + FE_FONT_H ? b : y + FE_FONT_H;
}

static int set_row_y(int idx)
{
   /* 13 rows must fit between the header (30) and footer (252) — the Back
    * row paid for one of the Theme/FPS additions (O exits, it was pure
    * redundancy) and the pixel budget paid for the other: headers advance
    * 19 px, items 16.  Last row lands at y=235, text ends at 251 — 1 px
    * clear of the footer.  (History: the page now scrolls, see above.)  The
    * selection band is SET_BAND_H = 20 on this 16 px pitch, so it covers
    * the 4 empty top rows of the next row's line box and the previous row's
    * descender tips (y+16..y+17); no capital of a neighbour is under it. */
   int y = SET_VIEW_TOP, i;
   for (i = 0; i < idx; i++)
      if (set_row_shown(i))
         y += set_rows[i].header ? (FE_FONT_H + 3) : FE_FONT_H;
   return y;
}

static void set_cursor_step(int dir)
{
   do
      g_cursor = (g_cursor + SET_COUNT + dir) % SET_COUNT;
   while (set_rows[g_cursor].header || !set_row_enabled(g_cursor));
}

/* Follow the cursor, and pull the section header in with the first item
 * under it -- an item that scrolls in headerless reads as orphaned. */
static void set_scroll_follow(void)
{
   int top = set_row_y(g_cursor);
   int bot = set_row_bot(top);
   int max = set_row_bot(set_row_y(SET_COUNT - 1)) - SET_VIEW_BOT;

   if (g_cursor > 0 && set_rows[g_cursor - 1].header)
      top = set_row_y(g_cursor - 1);
   if (max < 0)
      max = 0;
   if (top - g_set_scroll < SET_VIEW_TOP)
      g_set_scroll = top - SET_VIEW_TOP;
   if (bot - g_set_scroll > SET_VIEW_BOT)
      g_set_scroll = bot - SET_VIEW_BOT;
   if (g_set_scroll > max)
      g_set_scroll = max;
   if (g_set_scroll < 0)
      g_set_scroll = 0;
}

/* ----- link mode (docs/GBA-LINK-UI.md) -------------------------------------
 *
 * THE GAME DECIDES (link_games.h), the player overrides it per game
 * (fe_linkmode.h, roms/linkmode.txt), and the Wireless menus only ever ask
 * the HOST, and only for a game that links both ways. */

/* The file's text, for its one read and each write (BSS, never the heap). */
static char g_lm_text[FE_LINKMODE_MAX * (FE_LINKMODE_NAME + 12)];

static int lm_path(char *out, size_t cap)
{
   int n;
   if (!g_lm_dir[0])
      return -1;
   n = snprintf(out, cap, "%s/linkmode.txt", g_lm_dir);
   return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

void ui_linkmode_load(const char *rom_dir)
{
   char *text = g_lm_text;
   char path[PSP_FILE_PATH_CAP];
   SceUID fd;
   int len = 0;
   if (!rom_dir)
      return;
   snprintf(g_lm_dir, sizeof(g_lm_dir), "%s", rom_dir);
   fe_linkmode_clear(&g_lm);
   if (lm_path(path, sizeof(path)) == 0 &&
       (fd = sceIoOpen(path, PSP_O_RDONLY, 0)) >= 0)
   {
      len = sceIoRead(fd, text, sizeof(g_lm_text));
      sceIoClose(fd);
      if (len > 0)
         fe_linkmode_parse(&g_lm, text, (size_t)len);
   }
   g_lm_loaded = 1;
   fe_evt("linkmode_load n=%u bytes=%d", g_lm.n, len);
}

static int lm_save(void)
{
   char *text = g_lm_text;
   char path[PSP_FILE_PATH_CAP];
   size_t n = fe_linkmode_serialise(&g_lm, text, sizeof(g_lm_text));
   SceUID fd;
   int rc = 0;
   if (n >= sizeof(g_lm_text) || lm_path(path, sizeof(path)) != 0)
      return -1;
   fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd < 0 || sceIoWrite(fd, text, (int)n) != (int)n)
      rc = -1;
   if (fd >= 0)
      sceIoClose(fd);
   fe_evt("linkmode_save n=%u rc=%d", g_lm.n, rc);
   return rc;
}

void ui_set_game(const char *gba_code, const char *gb_title, const char *key,
                 int cable_ready)
{
   snprintf(g_game_code, sizeof(g_game_code), "%.4s",
            gba_code ? gba_code : "");
   snprintf(g_game_gbtitle, sizeof(g_game_gbtitle), "%.16s",
            gb_title ? gb_title : "");
   snprintf(g_game_key, sizeof(g_game_key), "%s", key ? key : "");
   g_cable_ready = cable_ready;
   ov_title_take();                 /* the LINKING screen's game line */
   fe_evt("link_game code=%s key=\"%s\" table=%d override=%d cable_ready=%d",
          g_game_code, g_game_key, link_game_mode(g_game_code),
          fe_linkmode_get(&g_lm, g_game_key), cable_ready);
}

/* The game Settings > Link mode edits: the running one, or in the browser
 * the highlighted one (browser_settings filled g_bset_*). */
static int lm_target(const char **key, const char **code)
{
   if (g_ov_browse)
   {
      *key = g_bset_key;
      *code = g_bset_code;
      return g_bset_have && g_pcfg.console == FE_CONSOLE_GBA;
   }
   *key = g_game_key;
   *code = g_game_code;
   return pcfg_display_console() == FE_CONSOLE_GBA && g_game_key[0];
}

static int linkmode_row_enabled(void)
{
   const char *k, *c;
   return lm_target(&k, &c);
}

static const char *linkmode_value(void)
{
   const char *k, *c;
   int ov;
   if (!lm_target(&k, &c))
      return g_ov_browse && g_pcfg.console == FE_CONSOLE_GBA ? "pick a game"
           : pcfg_display_console() != FE_CONSOLE_GBA ? "Link Cable"
                                                      : "Auto";
   ov = fe_linkmode_get(&g_lm, k);
   if (ov == FE_LM_CABLE)
      return "Link Cable";
   if (ov == FE_LM_ADAPTER)
      return "Wireless Adapter";
   switch (link_game_mode(c))
   {
   case LINK_ADAPTER: return "Auto: Wireless Adapter";
   case LINK_BOTH:    return "Auto: ask";
   default:           return "Auto: Link Cable";
   }
}

/* Auto -> Link Cable -> Wireless Adapter -> Auto (dir < 0 the other way). */
static void linkmode_cycle(int dir)
{
   const char *k, *c;
   int ov;
   if (!lm_target(&k, &c))
      return;
   ov = (fe_linkmode_get(&g_lm, k) + 3 + (dir < 0 ? -1 : 1)) % 3;
   if (fe_linkmode_set(&g_lm, k, ov) != 0)
      osd_toast("The link mode list is full");
   else
      (void)lm_save();
   fe_evt("linkmode_set key=\"%s\" mode=%d", k, ov);
}

/* What Host does for the running game (ui_psp.h). */
int ui_host_mode(void)
{
   int m;
   if (g_pcfg.console != FE_CONSOLE_GBA)
      return UI_HOST_CABLE;                    /* the GB link is a cable */
   m = fe_linkmode_effective(link_game_mode(g_game_code),
                             fe_linkmode_get(&g_lm, g_game_key));
   if (!g_cable_ready)
      return UI_HOST_ADAPTER;                  /* no mirror in this boot */
   return m == LINK_ADAPTER ? UI_HOST_ADAPTER
        : m == LINK_BOTH    ? UI_HOST_ASK : UI_HOST_CABLE;
}

int ui_link_mode_for(const char *code, const char *key)
{
   return fe_linkmode_effective(link_game_mode(code),
                                fe_linkmode_get(&g_lm, key));
}

int ui_host_cable(void)
{
   return g_host_cable;
}

void ui_set_link(const ui_link_view *v)
{
   if (!v)
   {
      memset(&g_link, 0, sizeof(g_link));
      return;
   }
   g_link = *v;
   snprintf(g_link_room, sizeof(g_link_room), "%s", v->room ? v->room : "");
   snprintf(g_link_game, sizeof(g_link_game), "%s", v->game ? v->game : "");
   g_link.room = g_link_room;
   g_link.game = g_link_game;
}

/* Which kinds of room this game can join (Join follows the host's room;
 * the override only widens what an adapter/cable-only game may enter, so a
 * hack the table gets wrong is not locked out). */
static int join_can_cable(void)
{
   if (g_pcfg.console != FE_CONSOLE_GBA)
      return 1;
   return g_cable_ready &&
          (link_game_mode(g_game_code) != LINK_ADAPTER ||
           fe_linkmode_get(&g_lm, g_game_key) == FE_LM_CABLE);
}

static int join_can_adapter(void)
{
   if (g_pcfg.console != FE_CONSOLE_GBA)
      return 0;
   return !g_cable_ready || link_game_mode(g_game_code) != LINK_CABLE ||
          fe_linkmode_get(&g_lm, g_game_key) == FE_LM_ADAPTER;
}

/* A ROM file's name as a sentence wants it: "Advance Wars 2 - Black Hole
 * Rising (USA)" -> "Advance Wars 2", "Pokemon - Crystal Version" ->
 * "Pokemon Crystal" (the design's short form): the region tags go, and the
 * part before " - " stays -- with the subtitle, minus " Version", when the
 * part before is only the series name Pokemon. */
static void short_game_name(const char *full, char *out, size_t cap)
{
   char t[100], *p, *sub = NULL;
   size_t n;
   snprintf(t, sizeof(t), "%s", full ? full : "");
   p = strpbrk(t, "([");
   if (p)
      *p = '\0';
   p = strstr(t, " - ");
   if (p)
   {
      *p = '\0';
      sub = p + 3;
   }
   for (n = strlen(t); n && t[n - 1] == ' '; n--)
      t[n - 1] = '\0';
   if (sub && !strcasecmp(t, "Pokemon"))
   {
      char *v = strstr(sub, " Version");
      if (v)
         *v = '\0';
      for (n = strlen(sub); n && sub[n - 1] == ' '; n--)
         sub[n - 1] = '\0';
      snprintf(out, cap, "%s %s", t, sub);
   }
   else
      snprintf(out, cap, "%s", t);
}

/* The game's name for screens: the table's, else the ROM's own, short. */
static const char *own_game_name(void)
{
   static char nm[64];
   const char *t = link_game_title(g_game_code);
   if (t)
      return t;
   short_game_name(g_ov_title, nm, sizeof(nm));
   return nm;
}

/* One scan row, decided: its columns and whether X joins it.  The plate
 * (why not) is written when it cannot be joined. */
typedef struct
{
   char mode[32], title[64], plate[96];
   int ok;
} scan_row;

static void scan_row_decide(const char *group, scan_row *r)
{
   char room[ROOM_LABEL_CAP], code[5];
   int num, kind = room_parse(group, &num, code), gba = g_pcfg.console ==
                                                         FE_CONSOLE_GBA;
   room_label(group, room, sizeof(room));
   memset(r, 0, sizeof(*r));
   r->ok = 1;
   /* "Share game data" rooms (docs/SAVE-SHARE.md) are joined from the game
    * list and nowhere else; link rooms need their game open.  Either way
    * the row is listed, and says why it cannot be joined from here. */
   if (kind == ROOM_SHARE)
   {
      snprintf(r->mode, sizeof(r->mode), "Game data");
      snprintf(r->title, sizeof(r->title), "saves and states");
      if (!g_ov_browse)
      {
         r->ok = 0;
         snprintf(r->plate, sizeof(r->plate),
                  "%s is sharing game data. Join it from the game list.",
                  room);
      }
      return;
   }
   if (g_ov_browse)
   {
      snprintf(r->mode, sizeof(r->mode), "%s",
               kind == ROOM_ADAPTER ? "Wireless Adapter" :
               kind == ROOM_OTHER ? "" : "Link Cable");
      snprintf(r->title, sizeof(r->title), "a game link");
      r->ok = 0;
      snprintf(r->plate, sizeof(r->plate),
               "%s is a link room. Open the game first, then join.", room);
      return;
   }
   switch (kind)
   {
   case ROOM_ADAPTER:
      if (gba)
      {
         snprintf(r->mode, sizeof(r->mode), "Wireless Adapter");
         if (!join_can_adapter())
         {
            r->ok = 0;
            snprintf(r->plate, sizeof(r->plate),
                     "%s is a Wireless Adapter room. This game links by "
                     "cable.", room);
         }
      }
      else
      {
         /* An older (3.1 beta) GB-link host advertised GPSPnn too: let the
          * handshake decide (owner, 2026-10-03). */
         snprintf(r->mode, sizeof(r->mode), "older GBAdhoc");
         snprintf(r->title, sizeof(r->title), "GB link or GBA adapter");
      }
      break;
   case ROOM_CABLE_GBA:
   {
      const char *t = !strcmp(code, g_game_code) ? own_game_name()
                                                 : link_game_title(code);
      snprintf(r->mode, sizeof(r->mode), "Link Cable");
      if (t)
         snprintf(r->title, sizeof(r->title), gba ? "%.56s" : "%.50s (GBA)", t);
      else
         snprintf(r->title, sizeof(r->title), "another game (%s)", code);
      if (!gba)
      {
         r->ok = 0;
         snprintf(r->plate, sizeof(r->plate),
                  "%s is a Game Boy Advance room.", room);
      }
      else if (strcmp(code, g_game_code) != 0)
      {
         char tt[64];
         r->ok = 0;
         snprintf(tt, sizeof(tt), "%.60s", t ? t : r->title);
         clip_title(tt, 240);
         snprintf(r->plate, sizeof(r->plate),
                  t ? "%s is playing %s. Open it to join."
                    : "%s is playing %s.", room, tt);
      }
      else if (!join_can_cable())
      {
         r->ok = 0;
         snprintf(r->plate, sizeof(r->plate), g_cable_ready
                  ? "%s is a Link Cable room. This game links by adapter."
                  : "%s is a Link Cable room. Restart the game to link.",
                  room);
      }
      break;
   }
   case ROOM_CABLE_GB:
      snprintf(r->mode, sizeof(r->mode), "Link Cable");
      snprintf(r->title, sizeof(r->title), "Game Boy / Color");
      if (gba)
      {
         r->ok = 0;
         snprintf(r->plate, sizeof(r->plate), "%s is a Game Boy room.", room);
      }
      break;
   default:
      break;                     /* verbatim; the handshake decides */
   }
}

static int g_profile_changed;

/* me_mode_name() was here.  It rendered the Media Engine row's value and
 * went with the row; the mode itself is still read from config.ini and still
 * falls back automatically.  See the SET_PROFILE note above the enum. */

static void settings_adjust(int id, int dir)
{
   switch (id)
   {
   case SET_SCALE:
      g_pcfg.scale = (g_pcfg.scale + VID_SCALE_MODES + dir) % VID_SCALE_MODES;
      pcfg_display_commit();
      vid_set_mode(g_pcfg.scale, g_pcfg.filter);
      fe_evt("video_mode scale=%s filter=%s profile=%s",
             vid_scale_name(g_pcfg.scale), vid_filter_name(g_pcfg.filter),
             pcfg_console_tag(pcfg_display_console()));
      break;
   case SET_FILTER:
      /* nearest -> bilinear -> sharp bilinear (docs/SHARP-BILINEAR.md). */
      g_pcfg.filter = (g_pcfg.filter + VID_FILTER_MODES + dir) %
                      VID_FILTER_MODES;
      pcfg_display_commit();
      vid_set_mode(g_pcfg.scale, g_pcfg.filter);
      fe_evt("video_mode scale=%s filter=%s profile=%s",
             vid_scale_name(g_pcfg.scale), vid_filter_name(g_pcfg.filter),
             pcfg_console_tag(pcfg_display_console()));
      break;
   case SET_AMBIENT:
      g_pcfg.ambient = (g_pcfg.ambient + PCFG_AMB_MODES + dir) % PCFG_AMB_MODES;
      pcfg_display_commit();
      vid_ambient_mode(g_pcfg.ambient);
      fe_evt("ambient_mode mode=%d profile=%s source=%d", g_pcfg.ambient,
             pcfg_console_tag(pcfg_display_console()), vid_ambient_source());
      break;
   case SET_OSD:
      g_pcfg.osd_wireless = !g_pcfg.osd_wireless;
      osd_session_chip_refresh();
      break;
   case SET_LINKMODE:
      /* linkmode.txt, not CONFIG.INI: written here, at once */
      linkmode_cycle(dir);
      return;
   case SET_FASTLINK:
      /* read when the next session starts (main_psp.c gblink_begin) */
      g_pcfg.fast_link = !g_pcfg.fast_link;
      fe_evt("fast_link_set on=%d", g_pcfg.fast_link);
      break;
   case SET_THEME:
      g_pcfg.theme = !g_pcfg.theme;   /* applies live — page() re-reads it */
      break;
   case SET_SHELL:
      g_pcfg.ui_shell = !g_pcfg.ui_shell;
      break;
   case SET_FPS:
      g_pcfg.show_fps = !g_pcfg.show_fps;
      break;
#ifdef REWIND
   case SET_REWIND:
      /* Takes effect when a game is next started: the memory is taken, and
       * the store paths built, before the cartridge is loaded. */
      g_pcfg.rewind = !g_pcfg.rewind;
      fe_evt("rewind_setting on=%d mb=%d", g_pcfg.rewind, g_pcfg.rewind_mb);
      break;
   case SET_REWIND_MB:
      /* Short / Medium / Long (config_psp.h) */
      g_pcfg.rewind_mb = pcfg_rewind_mb_step(g_pcfg.rewind_mb, dir);
      fe_evt("rewind_setting on=%d mb=%d", g_pcfg.rewind, g_pcfg.rewind_mb);
      break;
#endif
   case SET_STICK:
      /* off -> moves -> custom, live: the main loop reads it every frame. */
      g_pcfg.stick = (g_pcfg.stick + 3 + (dir < 0 ? -1 : 1)) % 3;
      fe_evt("ctl_stick mode=%s", ctl_stick_name(g_pcfg.stick));
      break;
   case SET_GBPAL:
   {
      /* Applies to a running GB game at once (and to the next boot); an
       * out-of-range id read from config.ini counts as Auto (0).  Per
       * console: GB and GBC keep their own; a GBA profile has none. */
      int n = fe_host_gb_palette_count();
      int cur = g_pcfg.gb_palette < n ? g_pcfg.gb_palette : 0;
      if (!set_row_enabled(SET_GBPAL))
         return;
      g_pcfg.gb_palette = (cur + n + dir) % n;
      pcfg_display_commit();
      fe_host_gb_palette_set(g_pcfg.gb_palette);
      break;
   }
   case SET_FFMULT:
      pcfg_ff_set_mode((pcfg_ff_mode() + PCFG_FF_COUNT + dir) % PCFG_FF_COUNT);
      break;
   case SET_FFMODE:
      g_pcfg.ff_hold = !g_pcfg.ff_hold;
      break;
   case SET_ROOM:
   {
      int nn = 0;
      if (strlen(g_pcfg.group) == 6 && strncmp(g_pcfg.group, "GPSP", 4) == 0)
         nn = (g_pcfg.group[4] - '0') * 10 + (g_pcfg.group[5] - '0');
      nn = (nn + 100 + dir) % 100;
      snprintf(g_pcfg.group, sizeof(g_pcfg.group), "GPSP%02d", nn);
      break;
   }
   default:
      return;
   }
   g_settings_dirty = 1;
}

/* One settings row in the overlay: label at 30, value right-aligned at 440,
 * the cursor row on the left-bleed band. */
static void set_item(int y, int sel, int enabled, const char *label,
                     const char *value)
{
   if (sel)
      ov_band(y, SET_BAND_W, SET_BAND_H);
   vid_text(30, y, label, enabled ? (sel ? C_SEL : C_ITEM) : C_DIM);
   if (value)
      ov_text_right(440, y, value,
                    enabled ? (sel ? C_VALUE : C_ACCENT_DK) : C_DIM);
}

static ui_action screen_settings(unsigned edges)
{
   int i;

   if (set_rows[g_cursor].header || !set_row_enabled(g_cursor))
      g_cursor = SET_ROOM;
   if (edges & PSP_CTRL_UP)
      set_cursor_step(-1);
   if (edges & PSP_CTRL_DOWN)
      set_cursor_step(+1);
   if (edges & PSP_CTRL_LEFT)
      settings_adjust(g_cursor, -1);
   if (edges & PSP_CTRL_RIGHT)
      settings_adjust(g_cursor, +1);
   if ((edges & PSP_CTRL_CROSS) && g_cursor == SET_CONTROLS)
   {
      screen_to(SCR_CONTROLS);
      return screen_controls(0, 0);   /* draw this frame as the new page */
   }
   /* Share game data / the room list: the browser's Settings loop acts on
    * the request after this frame is drawn (browser_settings). */
   if ((edges & PSP_CTRL_CROSS) && g_ov_browse &&
       (g_cursor == SET_SHARE || g_cursor == SET_JOIN))
      g_share_req = g_cursor == SET_SHARE ? 1 : 2;
   else if (edges & PSP_CTRL_CROSS)
      settings_adjust(g_cursor, +1);
   if (edges & PSP_CTRL_CIRCLE)
   {
      /* ADR-0071: leaving SETTINGS after changing the profile is the commit
       * point.  The main loop saves and relaunches from there — not from
       * here — so the SRAM flush, thread teardown and log close all happen
       * exactly as they do on a normal exit.  Relaunching out of the UI with
       * the io thread still holding the .sav is how a save gets truncated. */
      if (g_profile_changed) { g_profile_changed = 0;
                               return UI_ACT_RELAUNCH; }
      screen_to(SCR_MENU);
   }

   /* Scrim 200 over the frame (the browser's page, from START there): a
    * long list needs more hold-back than the menu's 170. */
   ov_backdrop(OV_SCRIM_DENSE);
   ov_head(NULL, "SETTINGS");
   set_scroll_follow();
   vid_clip(0, SET_VIEW_TOP - 2, VID_SCR_W, SET_VIEW_BOT - SET_VIEW_TOP + 4);
   for (i = 0; i < SET_COUNT; i++)
   {
      int y = set_row_y(i) - g_set_scroll;
      if (!set_row_shown(i))
         continue;                /* a browser-only row, in game */
      if (y + FE_FONT_H <= SET_VIEW_TOP - 2 || y >= SET_VIEW_BOT + 2)
         continue;                /* wholly outside the scissor */
      if (set_rows[i].header)
      {
         int lx = 20 + vid_text_w(set_rows[i].label) + FE_FONT_W;
         vid_text(20, y, set_rows[i].label, C_ACCENT);
         if (i == SET_HDR_VID)
         {
            /* Which profile these rows edit, in that console's own accent
             * -- the colour its browser badge wears. */
            char tag[12];
            int tx = 20 + vid_text_w(set_rows[i].label) + 6;
            snprintf(tag, sizeof(tag), "(%s)",
                     pcfg_console_tag(pcfg_display_console()));
            vid_text(tx, y, tag, skin_of(pcfg_display_console())->accent);
            lx = tx + vid_text_w(tag) + FE_FONT_W;
         }
         vid_rect(lx, y + FE_FONT_H / 2, 440 - lx, 1, C_ACCENT_DK, 120);
         continue;
      }
      switch (i)
      {
      case SET_ROOM:
      {
         char lbl[ROOM_LABEL_CAP];
         set_item(y, g_cursor == i, 1, set_rows[i].label,
                  room_label(g_pcfg.group, lbl, sizeof(lbl)));
         break;
      }
      case SET_LINKMODE:
         set_item(y, g_cursor == i, set_row_enabled(i), set_rows[i].label,
                  linkmode_value());
         break;
      case SET_OSD:
         set_item(y, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.osd_wireless ? "shown" : "hidden");
         break;
      case SET_SHARE:
      {
         char lbl[ROOM_LABEL_CAP], v[48];
         snprintf(v, sizeof(v), "host saves and states on %s",
                  room_label(g_pcfg.group, lbl, sizeof(lbl)));
         set_item(y, g_cursor == i, 1, set_rows[i].label, v);
         break;
      }
      case SET_JOIN:
         set_item(y, g_cursor == i, 1, set_rows[i].label, NULL);
         break;
      case SET_FASTLINK:
         set_item(y, g_cursor == i, set_row_enabled(i), set_rows[i].label,
                  !set_row_enabled(i) ? "GBA Link Cable only"
                  : g_pcfg.fast_link ? "on" : "off");
         break;
      case SET_SCALE:
         set_item(y, g_cursor == i, 1, set_rows[i].label,
             vid_scale_name(g_pcfg.scale));
         break;
      case SET_FILTER:
         /* 2x samples nearest whatever this says; say so. */
         set_item(y, g_cursor == i, g_pcfg.scale != VID_SCALE_INT2,
             set_rows[i].label,
             g_pcfg.scale == VID_SCALE_INT2 ? "sharp (2x)"
                                            : vid_filter_name(g_pcfg.filter));
         break;
      case SET_AMBIENT:
         set_item(y, g_cursor == i, 1, set_rows[i].label,
             pcfg_ambient_name(g_pcfg.ambient));
         break;
      case SET_FFMULT:
         set_item(y, g_cursor == i, 1, set_rows[i].label, pcfg_ff_name());
         break;
      case SET_FFMODE:
      {
         /* Names the button it is about, which is no longer always
          * Square; with the default binding this reads exactly as 3.0. */
         char lbl[48], b[32];
         unsigned ffb = g_pcfg.controls.bind[CTL_SC_FF];
         if (ffb == CTL_SQUARE)
            snprintf(lbl, sizeof(lbl), "%s", set_rows[i].label);
         else
         {
            ctl_format(ffb, b, sizeof(b), CTL_UI_NAMES, "none");
            snprintf(lbl, sizeof(lbl), "FF button (%s)", b);
         }
         set_item(y, g_cursor == i, 1, lbl,
             g_pcfg.ff_hold ? "hold" : "toggle");
         break;
      }
      case SET_CONTROLS:
         set_item(y, g_cursor == i, 1, set_rows[i].label,
             ctl_is_default(&g_pcfg.controls)
                ? "default" : "custom");
         break;
      case SET_THEME:
         set_item(y, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.theme ? "light" : "dark");
         break;
      case SET_SHELL:
         set_item(y, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.ui_shell ? "marquee" : "shelf");
         break;
      case SET_FPS:
         set_item(y, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.show_fps ? "on" : "off");
         break;
#ifdef REWIND
      case SET_REWIND:
         set_item(y, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.rewind ? "on (from the next game)"
                           : "off");
         break;
      case SET_REWIND_MB:
      {
         /* A name, not a number of seconds: the history target is off in
          * this build (psp_rewind_product), so how far back a megabyte
          * reaches depends on the scene.  The megabytes are in the footer
          * while the cursor is here. */
         set_item(y, g_cursor == i, g_pcfg.rewind, set_rows[i].label,
                  pcfg_rewind_mb_name(g_pcfg.rewind_mb));
         break;
      }
#endif
      case SET_STICK:
         set_item(y, g_cursor == i, 1, set_rows[i].label,
             g_pcfg.stick == CTL_STICK_MOVE ? "moves (as the d-pad)" :
             g_pcfg.stick == CTL_STICK_CUSTOM ? "custom (see Button mapping)"
                                              : "off");
         break;
      case SET_GBPAL:
      {
         int gp = g_pcfg.gb_palette < fe_host_gb_palette_count()
                  ? g_pcfg.gb_palette : 0;
         int en = set_row_enabled(i);
         set_item(y, g_cursor == i, en, set_rows[i].label,
             en ? fe_host_gb_palette_name(gp) : "GB / GBC only");
         break;
      }
      }
   }
   /* The relaunch-to-apply footer went with the Media Engine row: nothing
    * sets g_profile_changed any more, so the branch could never be taken and
    * the text advertised a setting that no longer exists.  The mechanism
    * itself stays for the next setting that needs a reboot. */
   vid_clip_off();
   ov_scrollbar(470, SET_VIEW_TOP, SET_VIEW_BOT - SET_VIEW_TOP, g_set_scroll,
                SET_VIEW_BOT - SET_VIEW_TOP,
                set_row_bot(set_row_y(SET_COUNT - 1)) - SET_VIEW_TOP);
   if (g_cursor == SET_FASTLINK)
      footer("For a strong connection. The host's setting is used.");
#ifdef REWIND
   else if (g_cursor == SET_REWIND_MB)
      /* What each length is, and what the largest needs.  Nothing is
       * refused on a PSP-1000: it gets what its memory can give. */
      footer(pcfg_rewind_mb_snap(g_pcfg.rewind_mb) == PCFG_REWIND_MB_SHORT
                ? "Short: 1 MB of history. The lightest on memory."
             : pcfg_rewind_mb_snap(g_pcfg.rewind_mb) == PCFG_REWIND_MB_MEDIUM
                ? "Medium: 2 MB of history."
                : "Long: 4 MB. Needs the extra memory of a PSP-2000, 3000 or Go.");
#endif
   else
      footer("DPAD move/change   " VID_GLYPH_X " select   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* ----- controls screen (Settings > Controls) ------------------------------
 *
 * docs/CONTROL-REMAP.md.  MODEL AND VIEW ARE SEPARATE: every rule -- what an
 * action may be bound to, the conflict rule, the defaults, CONFIG.INI -- is
 * ctl_map.c, which the host test exercises.  This section only walks rows,
 * captures a press and draws, in the overlay's language (docs/UI-OVERLAY.md):
 * two labelled columns, GAME BUTTONS and SHORTCUTS, each binding a row of
 * CHIPS; the fixed chords are locked chips the cursor skips; what the page
 * has to SAY (capture, a conflict, a refusal) is a plate where the footer
 * was.
 *
 * LOCKOUT SAFETY.  Nothing on this page goes through a game binding: it is
 * driven by the fixed menu keys (d-pad, X, O, and Square/Triangle as page
 * actions), and it is opened through START+SELECT, which no binding can
 * take.  Any mapping, however wrong, can be undone from here. */

enum { CR_ACT, CR_FIXED, CR_RESET, CR_MENU, CR_TURBO };
typedef struct
{
   unsigned char kind;
   unsigned char col;      /* 0 GAME BUTTONS, 1 SHORTCUTS                   */
   signed char   act;      /* CR_ACT: the ctl_map action                    */
   const char   *label;    /* CR_FIXED / CR_RESET                           */
   const char   *chord;    /* CR_FIXED: its one locked chip, forever        */
} ctl_row;

/* In CURSOR order, which is column-major: down the game buttons, then down
 * the shortcuts -- the same sequence the single list had, so UP/DOWN walk
 * every row exactly as before (and the harness walker's arithmetic holds).
 * LEFT/RIGHT cross between the columns. */
static const ctl_row CTL_ROWS[] = {
   { CR_ACT, 0, CTL_GAME_A, NULL, NULL },
   { CR_ACT, 0, CTL_GAME_B, NULL, NULL },
   { CR_ACT, 0, CTL_GAME_L, NULL, NULL },
   { CR_ACT, 0, CTL_GAME_R, NULL, NULL },
   { CR_ACT, 0, CTL_GAME_START, NULL, NULL },
   { CR_ACT, 0, CTL_GAME_SELECT, NULL, NULL },
   { CR_ACT, 0, CTL_GAME_UP, NULL, NULL },
   { CR_ACT, 0, CTL_GAME_DOWN, NULL, NULL },
   { CR_ACT, 0, CTL_GAME_LEFT, NULL, NULL },
   { CR_ACT, 0, CTL_GAME_RIGHT, NULL, NULL },
   /* The four turbo actions have a rate and a mode each, which a chip row
    * cannot hold, and the columns are full: this row opens their own page
    * (screen_turbo), still SCR_CONTROLS. */
   { CR_TURBO, 0, -1, "Turbo buttons", NULL },
   { CR_ACT, 1, CTL_SC_FF, NULL, NULL },
   { CR_ACT, 1, CTL_SC_VIDEO, NULL, NULL },
   { CR_ACT, 1, CTL_SC_SAVE, NULL, NULL },
   { CR_ACT, 1, CTL_SC_LOAD, NULL, NULL },
   { CR_ACT, 1, CTL_SC_SHOT, NULL, NULL },
   { CR_ACT, 1, CTL_SC_PAUSE, NULL, NULL },
   { CR_ACT, 1, CTL_SC_MGIFT, NULL, NULL },
   { CR_ACT, 1, CTL_SC_CATCH, NULL, NULL },     /* bisect builds only */
#ifdef REWIND
   { CR_ACT, 1, CTL_SC_REWIND, NULL, NULL },
#endif
   /* The menu button is a TOGGLE, not a binding: START+SELECT (hold) or HOME
    * (docs/CONTROL-REMAP.md section 10).  Nothing else can ever be chosen,
    * so no value of it can lock the player out of this page. */
   { CR_MENU, 1, -1, "Menu", NULL },
   /* Fixed.  Its words follow the menu row: with the menu on HOME, what a
    * player needs to read here is how to QUIT (ctl_fixed_row). */
   { CR_FIXED, 1, -1, "Home", "HOME" },
   { CR_RESET, 1, -1, "Reset to defaults", NULL },
};
#define CTL_ROWS_N ((int)(sizeof(CTL_ROWS) / sizeof(CTL_ROWS[0])))

/* The two columns: x, width, title (the design's numbers). */
static const struct { int x, w; const char *title; } CTL_COLS[2] = {
   {  20, 170, "GAME BUTTONS" },
   { 206, 254, "SHORTCUTS" },
};
/* Column titles at CTL_COL_Y; rows from CTL_ROW_Y0.  The design's 19 px
 * pitch fits its eight rows a column; the real table has ten (the d-pad is
 * remappable, and so is the pause screen), so the titles sit higher and the
 * pitch is whatever lands the last row's chip by CTL_LAST_Y + CHIP_H --
 * 18 px for ten rows, 19 at most, clear of the plate at 252. */
#define CTL_COL_Y   48
#define CTL_ROW_Y0  (CTL_COL_Y + 20)
#define CTL_LAST_Y  230
#define CTL_PITCH_MAX 19
/* Capture gives up after ~5 s with nothing pressed.  A press is the only
 * way to finish, because every button -- O included -- is a legal answer. */
#define CTL_CAP_TIMEOUT 300
/* The capture chip's pulse: alpha 255 -> 96 -> 255 every 48 frames. */
#define CTL_PULSE_FRAMES 48

static struct { int on, act, phase, timer; unsigned got; } g_cap;
static int  g_ctl_reset_armed;
static char g_ctl_note[112];
/* The Turbo buttons page: open, and its cell cursor (row = which turbo
 * action, col = button / rate / mode). */
static struct { int on, row, col; } g_tp;
/* A link or wireless session is up: turbo buttons do nothing (main_psp.c
 * turbo_frame), and both pages say so in the Cheats page's words. */
static int g_turbo_locked;
#define TURBO_LOCK_MSG \
   "Turbo is off during link play. It comes back when it ends."

#ifdef GPSP_PERF_RIG
/* Harness `ui_turbo_lock = 1`: the pages as a link session shows them, for a
 * screenshot (one PPSSPP cannot hold a session). */
static int g_turbo_lock_force;
void ui_turbo_lock_inject(int on) { g_turbo_lock_force = on ? 1 : 0; }
#else
#define g_turbo_lock_force 0
void ui_turbo_lock_inject(int on) { (void)on; }
#endif

void ui_set_turbo_locked(int on)
{
   g_turbo_locked = (on || g_turbo_lock_force) ? 1 : 0;
}

int ui_capturing(void)
{
   return g_cap.on;
}

static int ctl_cap_timer(void)
{
   return g_cap.on ? g_cap.timer : -1;
}

static void ctl_page_reset(void)
{
   g_cap.on = 0;
   g_ctl_reset_armed = 0;
   g_ctl_note[0] = '\0';
   g_tp.on = 0;
}

static int ctl_row_shown(int i)
{
   return CTL_ROWS[i].kind != CR_ACT || ctl_available(CTL_ROWS[i].act);
}

static int ctl_row_stop(int i)
{
   return ctl_row_shown(i) &&
          (CTL_ROWS[i].kind == CR_ACT || CTL_ROWS[i].kind == CR_RESET ||
           CTL_ROWS[i].kind == CR_MENU || CTL_ROWS[i].kind == CR_TURBO);
}

/* Index of row i within its column, counting shown rows only. */
static int ctl_row_pos(int idx)
{
   int i, n = 0;
   for (i = 0; i < idx; i++)
      if (CTL_ROWS[i].col == CTL_ROWS[idx].col && ctl_row_shown(i))
         n++;
   return n;
}

static int ctl_pitch(void)
{
   int n[2] = { 0, 0 }, i, most, p;
   for (i = 0; i < CTL_ROWS_N; i++)
      if (ctl_row_shown(i))
         n[CTL_ROWS[i].col]++;
   most = n[0] > n[1] ? n[0] : n[1];
   if (most < 2)
      return CTL_PITCH_MAX;
   p = (CTL_LAST_Y - CTL_ROW_Y0) / (most - 1);
   return p < CTL_PITCH_MAX ? p : CTL_PITCH_MAX;
}

static void ctl_cursor_step(int dir)
{
   int n = 0;
   do
      g_cursor = (g_cursor + CTL_ROWS_N + dir) % CTL_ROWS_N;
   while (!ctl_row_stop(g_cursor) && ++n < CTL_ROWS_N);
}

/* LEFT/RIGHT: the other column, same row -- or the nearest stop to it,
 * preferring the one above (the fixed rows are not stops). */
static void ctl_cursor_cross(void)
{
   int col = !CTL_ROWS[g_cursor].col, pos = ctl_row_pos(g_cursor);
   int best = -1, best_d = 1 << 20, i;
   for (i = 0; i < CTL_ROWS_N; i++)
   {
      int d;
      if (CTL_ROWS[i].col != col || !ctl_row_stop(i))
         continue;
      d = ctl_row_pos(i) - pos;
      d = d < 0 ? -2 * d : 2 * d + 1;
      if (d < best_d)
      {
         best_d = d;
         best = i;
      }
   }
   if (best >= 0)
      g_cursor = best;
}

static void ctl_value(unsigned mask, char *b, size_t n)
{
   ctl_format(mask, b, n, CTL_UI_NAMES, "none");
}

/* The button names of `mask`, in ctl_button_order() order -- the order
 * ctl_format() spells a chord in, so a chip row reads as the CONFIG.INI
 * value does. */
static int chord_parts(unsigned mask, const char **out)
{
   int i, n = 0;
   for (i = 0; i < CTL_NBUTTONS; i++)
      if (mask & ctl_button_order(i))
         out[n++] = CTL_UI_NAMES[i];
   return n;
}

/* The visible half of the conflict rule: whatever moved is said in words,
 * on the plate.  A plain rebind needs no words -- its chip shows it. */
static void ctl_note_result(int a, unsigned mask, int err, int stolen)
{
   char v[40];
   const char *lbl = ctl_info(a)->label;
   ctl_value(mask, v, sizeof(v));
   if (err != CTL_OK)
      snprintf(g_ctl_note, sizeof(g_ctl_note), "%s unchanged: %s", lbl,
               ctl_error_text(err));
   else if (stolen >= 0)
      snprintf(g_ctl_note, sizeof(g_ctl_note),
               "%s was %s  -  %s is now blank (disabled)", v,
               ctl_info(stolen)->label, ctl_info(stolen)->label);
   else
      g_ctl_note[0] = '\0';
}

/* THE ONE-TIME NOTE.  Quick save or Quick load on a stick direction alone
 * is the player's call -- and the one binding a console tossed on a bed can
 * fire by itself.  Said once, in the owner's words, never again
 * (CONFIG.INI `stick_warned`). */
static int g_ctl_warn;
#ifdef GPSP_PERF_RIG
/* Harness `ui_ctl_warn = 1`: put the note up, for a screenshot. */
void ui_ctl_warn_inject(int on) { g_ctl_warn = on ? 1 : 0; }
#else
void ui_ctl_warn_inject(int on) { (void)on; }
#endif
static const char *const CTL_WARN_LINES[] = {
   "Hey man, I'm not gonna tell you how to live your life",
   "but be careful. Last thing you'd want is to lose",
   "progress by accidentally saving or loading a state by",
   "doing something as simple as tossing your PSP on the",
   "bed. You won't see this again.",
};
#define CTL_WARN_N ((int)(sizeof(CTL_WARN_LINES) / sizeof(CTL_WARN_LINES[0])))

static void ctl_warn_draw(void)
{
   const int lh = FE_FONT_H + 2;
   const int w = 400, h = 16 + CTL_WARN_N * lh + 10 + lh + 12;
   const int x = (VID_SCR_W - w) / 2, y = (VID_SCR_H - h) / 2;
   int i;
   vid_rect(0, 0, VID_SCR_W, VID_SCR_H, C_BG_BOT, 120);
   vid_rect(x, y, w, h, C_BG_BOT, 245);
   vid_rect(x, y, w, 1, C_ACCENT, 255);
   vid_rect(x, y + h - 1, w, 1, C_ACCENT_DK, 255);
   for (i = 0; i < CTL_WARN_N; i++)
      vid_text_center(y + 16 + i * lh, CTL_WARN_LINES[i], C_ITEM);
   vid_text_center(y + 16 + CTL_WARN_N * lh + 10, VID_GLYPH_X " OK", C_SEL);
}

static void ctl_apply(int a, unsigned mask, const char *via)
{
   int stolen = -1, err = ctl_assign(&g_pcfg.controls, a, mask, &stolen);
   ctl_note_result(a, mask, err, stolen);
   if (err == CTL_OK)
      g_settings_dirty = 1;
   if (err == CTL_OK && (a == CTL_SC_SAVE || a == CTL_SC_LOAD) &&
       ctl_stick_only(mask) && !g_pcfg.stick_warned)
      g_ctl_warn = 1;
   FE_EVT_ONLY(via);
   fe_evt("ctl_bind key=%s mask=0x%04x err=%d stolen=%s via=%s",
          ctl_info(a)->key, mask, err,
          stolen >= 0 ? ctl_info(stolen)->key : "none", via);
}

static void ctl_capture_begin(int a)
{
   g_cap.on = 1;
   g_cap.act = a;
   g_cap.phase = 0;
   g_cap.timer = 0;
   g_cap.got = 0;
   g_ctl_note[0] = '\0';
   fe_evt("ctl_capture key=%s", ctl_info(a)->key);
}

/* One frame of capture.  Phase 0 waits for the X that opened it to come up;
 * phase 1 listens; phase 2 collects every button held until ALL are released
 * -- so a chord is captured whatever order its buttons went down in. */
static void ctl_capture_step(unsigned pad)
{
   unsigned p = pad & CTL_ALL;

   if (g_cap.phase == 0)
   {
      if (!p)
         g_cap.phase = 1;
   }
   else if (g_cap.phase == 1)
   {
      if (p)
      {
         g_cap.phase = 2;
         g_cap.got = p;
      }
   }
   else
      g_cap.got |= p;

   if (g_cap.phase == 2)
   {
      /* Refused at once rather than on release: holding START+SELECT for
       * 1.5 s is also the fixed "end the run" gesture (ADR-0057). */
      if ((g_cap.got & CTL_MENU_CHORD) == CTL_MENU_CHORD)
      {
         ctl_note_result(g_cap.act, g_cap.got, CTL_E_RESERVED, -1);
         g_cap.on = 0;
      }
      else if (!p)
      {
         ctl_apply(g_cap.act, g_cap.got, "capture");
         g_cap.on = 0;
      }
      return;
   }
   if (++g_cap.timer >= CTL_CAP_TIMEOUT)
   {
      snprintf(g_ctl_note, sizeof(g_ctl_note), "%s unchanged: nothing pressed",
               ctl_info(g_cap.act)->label);
      g_cap.on = 0;
   }
}

/* The capture chip's outline alpha this frame: a triangle wave from 255
 * down to 96 and back, starting at full.  Steady while a chord is held. */
static int ctl_pulse(void)
{
   int t = g_cap.timer % CTL_PULSE_FRAMES, half = CTL_PULSE_FRAMES / 2;
   int tri = t < half ? t * 256 / half : (CTL_PULSE_FRAMES - t) * 256 / half;
   if (g_cap.phase == 2)
      return 255;
   return 255 - ((255 - 96) * tri >> 8);
}

/* The capture plate.  Centred on the 5-second wording so the countdown's
 * digit changing cannot shift the line a pixel. */
static void ctl_capture_plate(void)
{
   const ctl_action_info *in = ctl_info(g_cap.act);
   char t[112], v[40];
   int secs = (CTL_CAP_TIMEOUT - g_cap.timer + 59) / 60;
   const char *ask = (in->flags & (CTL_F_GAME | CTL_F_TURBO))
                                              ? "Press a button for"
                                              : "Press a button or combo for";
   vid_rect(0, OV_PLATE_Y, VID_SCR_W, FTR_H, C_ACCENT, 40);
   if (g_cap.phase == 2)
   {
      ctl_value(g_cap.got, v, sizeof(v));
      snprintf(t, sizeof(t), "%s      let go to set", v);
      vid_text_center(OV_PLATE_Y + 2, t, C_SEL);
      return;
   }
   /* "O cancel" is not offered: O is a legal answer (A is on O by
    * default), so capture ends on any press or on the timeout. */
   snprintf(t, sizeof(t), "%s %s      cancels in 5 s", ask, in->label);
   {
      int x = (VID_SCR_W - vid_text_w(t)) / 2;
      snprintf(t, sizeof(t), "%s %s      cancels in %d s", ask, in->label,
               secs);
      vid_text(x, OV_PLATE_Y + 2, t, C_SEL);
   }
}

/* A fixed row's label, chip and tag.  The "Home" row says what HOME does,
 * which the menu toggle changes: with the menu on HOME it becomes the row
 * that says how to QUIT, because the system's exit dialog is not on HOME in
 * game any more.  Returns the label; chip/tag may be NULL. */
static const char *ctl_fixed_row(const ctl_row *ri, const char **chip,
                                 const char **tag)
{
   int quit = g_pcfg.controls.menu_home && ri->chord &&
              strcmp(ri->chord, "HOME") == 0;
   if (chip)
      *chip = quit ? "START+SELECT" : ri->chord;
   if (tag)
      *tag = quit ? "hold 1.5 s" : "fixed";
   return quit ? "Quit" : ri->label;
}

/* ----- the Turbo buttons page (docs/CONTROL-REMAP.md section 13) -----------
 * Four rows, three cells each: the BUTTON (a chip, captured exactly as a
 * game button is), the RATE and the MODE.  The d-pad moves the cell cursor;
 * the page keys are the Controls page's own: X changes the cell (rebind /
 * next rate / other mode), Triangle puts it back to its default, Square
 * blanks a button, O returns to the Controls page.  It is drawn by
 * screen_controls, so capture, the plate and both ways in (in game, from the
 * browser) are the ones that page already has. */
#define TP_ROW_Y0  92
#define TP_PITCH   22
#define TP_HELP_Y  188
static const struct { int xr; const char *head; } TP_CELLS[3] = {
   { 250, "Button" }, { 350, "Rate" }, { 450, "Mode" },
};
static const char *const TP_HELP[3][2] = {
   { "A turbo button presses A (or B) over and over for you.",
     "Each of the four has its own button, rate and mode." },
   { "Presses a second: 10, 15 or 30.",
     "Counted in game frames, so fast-forward speeds it up too." },
   { "Hold: fires for as long as you hold the button.",
     "Toggle: press to start, press again to stop." },
};

static int tp_bound_count(void)
{
   int i, n = 0;
   for (i = 0; i < CTL_TURBO_N; i++)
      if (g_pcfg.controls.bind[CTL_TURBO_FIRST + i])
         n++;
   return n;
}

static void tp_set(int i, int half, int toggle)
{
   ctl_map *m = &g_pcfg.controls;
   if (m->turbo_half[i] == half && m->turbo_toggle[i] == toggle)
      return;
   m->turbo_half[i] = (uint8_t)half;
   m->turbo_toggle[i] = (uint8_t)toggle;
   g_settings_dirty = 1;
   fe_evt("ctl_turbo key=%s rate=%d mode=%s",
          ctl_info(CTL_TURBO_FIRST + i)->key, ctl_turbo_rate(half),
          ctl_turbo_mode_name(toggle));
}

static ui_action screen_turbo(unsigned edges)
{
   ctl_map *m = &g_pcfg.controls;
   const char *parts[CTL_NBUTTONS];
   int i, c, a;

   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
      g_ctl_note[0] = '\0';       /* read; the plate gives the footer back */
   if (edges & PSP_CTRL_UP)
      g_tp.row = (g_tp.row + CTL_TURBO_N - 1) % CTL_TURBO_N;
   if (edges & PSP_CTRL_DOWN)
      g_tp.row = (g_tp.row + 1) % CTL_TURBO_N;
   if (edges & PSP_CTRL_LEFT)
      g_tp.col = (g_tp.col + 2) % 3;
   if (edges & PSP_CTRL_RIGHT)
      g_tp.col = (g_tp.col + 1) % 3;
   if (edges & PSP_CTRL_CIRCLE)
   {
      g_tp.on = 0;                /* back to the row that opened it */
      g_ctl_note[0] = '\0';
      return screen_controls(0, 0);
   }
   i = g_tp.row;
   a = CTL_TURBO_FIRST + i;
   if (g_tp.col == 0)
   {
      if (edges & PSP_CTRL_CROSS)
         ctl_capture_begin(a);
      else if (edges & PSP_CTRL_SQUARE)
         ctl_apply(a, 0, "none");
      else if (edges & PSP_CTRL_TRIANGLE)
         ctl_apply(a, ctl_default(a), "default");
   }
   else if (g_tp.col == 1)
   {
      if (edges & PSP_CTRL_CROSS)
         tp_set(i, ctl_turbo_half_step(m->turbo_half[i], +1),
                m->turbo_toggle[i]);
      else if (edges & PSP_CTRL_TRIANGLE)
         tp_set(i, CTL_TURBO_HALF_DEF, m->turbo_toggle[i]);
   }
   else
   {
      if (edges & PSP_CTRL_CROSS)
         tp_set(i, m->turbo_half[i], !m->turbo_toggle[i]);
      else if (edges & PSP_CTRL_TRIANGLE)
         tp_set(i, m->turbo_half[i], 0);
   }

   ov_backdrop(OV_SCRIM_DENSE);
   ov_head(NULL, "CONTROLS");
   {
      int lx = 20 + vid_text_w("TURBO BUTTONS") + FE_FONT_W;
      vid_text(20, CTL_COL_Y, "TURBO BUTTONS", C_ACCENT);
      vid_rect(lx, CTL_COL_Y + FE_FONT_H / 2, 450 - lx, 1, C_ACCENT_DK, 120);
   }
   for (c = 0; c < 3; c++)
      ov_text_right(TP_CELLS[c].xr, CTL_COL_Y + 20, TP_CELLS[c].head, C_DIM);
   for (i = 0; i < CTL_TURBO_N; i++)
   {
      int y = TP_ROW_Y0 + i * TP_PITCH, sel = (i == g_tp.row);
      int cy = vid_band_y(y, CHIP_H);
      char rate[16];
      a = CTL_TURBO_FIRST + i;
      if (sel)
      {
         int by = vid_band_y(y, TP_PITCH);
         vid_rect(12, by, 446, TP_PITCH, C_ACCENT, 40);
         vid_rect(12, by, 3, TP_PITCH, C_ACCENT, 255);
      }
      vid_text(24, y, ctl_info(a)->label, sel ? C_SEL : C_ITEM);
      for (c = 0; c < 3; c++)
      {
         int xr = TP_CELLS[c].xr, w, n;
         if (c == 0 && g_cap.on && g_cap.act == a)
         {
            if (g_cap.phase == 2 && g_cap.got)
               n = chord_parts(g_cap.got, parts);
            else
            {
               parts[0] = "press...";
               n = 1;
            }
            w = combo_right(xr, y, parts, n, CHIP_CAPTURE, ctl_pulse());
         }
         else if (c == 0)
         {
            n = chord_parts(m->bind[a], parts);
            w = combo_right(xr, y, parts, n,
                            g_turbo_locked ? CHIP_LOCKED : CHIP_BOUND, 255);
         }
         else
         {
            const char *v = rate;
            if (c == 1)
               snprintf(rate, sizeof(rate), "%d / sec",
                        ctl_turbo_rate(m->turbo_half[i]));
            else
               v = m->turbo_toggle[i] ? "Toggle" : "Hold";
            w = chip_w(v);
            chip(xr - w, y, v, CHIP_BOUND, 255);
         }
         /* The cell the keys act on: an outline two pixels off its chip. */
         if (sel && c == g_tp.col && !g_cap.on)
            ov_outline(xr - w - 2, cy - 2, w + 4, CHIP_H + 4, C_ACCENT, 255);
      }
   }
   vid_text(24, TP_HELP_Y, TP_HELP[g_tp.col][0], C_DIM);
   vid_text(24, TP_HELP_Y + FE_FONT_H + 2, TP_HELP[g_tp.col][1], C_DIM);

   if (g_cap.on)
      ctl_capture_plate();
   else if (g_ctl_note[0])
      ov_plate(g_ctl_note);
   else if (g_turbo_locked)
      ov_plate(TURBO_LOCK_MSG);
   else if (g_tp.col == 0)
      footer(VID_GLYPH_X " rebind   " VID_GLYPH_SQ " none   "
             VID_GLYPH_TRI " default   " VID_GLYPH_O " back      "
             "blank = disabled");
   else
      footer(VID_GLYPH_X " change   " VID_GLYPH_TRI " default   "
             VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

static ui_action screen_controls(unsigned edges, unsigned pad)
{
   const ctl_row *r;
   const char *parts[CTL_NBUTTONS];
   int i, c, pitch;

   if (g_ctl_warn)
   {
      /* The note owns the pad: X (or O) puts it away for good. */
      if (edges & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE))
      {
         g_ctl_warn = 0;
         g_pcfg.stick_warned = 1;
         g_settings_dirty = 1;
         fe_evt("ctl_stick_warned");
      }
      edges = 0;
   }
   else if (g_cap.on)
   {
      ctl_capture_step(pad);
      edges = 0;                  /* capture owns the pad this frame */
   }
   if (g_tp.on && !g_ctl_warn)
      return screen_turbo(edges);
   if (!ctl_row_stop(g_cursor))
      ctl_cursor_step(+1);
   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
   {
      g_ctl_reset_armed = 0;
      g_ctl_note[0] = '\0';       /* read; the plate gives the footer back */
   }
   if (edges & PSP_CTRL_UP)
      ctl_cursor_step(-1);
   if (edges & PSP_CTRL_DOWN)
      ctl_cursor_step(+1);
   if (edges & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
      ctl_cursor_cross();
   r = &CTL_ROWS[g_cursor];

   if (edges & PSP_CTRL_CIRCLE)
   {
      screen_to(SCR_SETTINGS);    /* saves the bindings if they changed */
      g_cursor = SET_CONTROLS;
      return screen_settings(0);
   }
   if (r->kind == CR_ACT)
   {
      if (edges & PSP_CTRL_CROSS)
         ctl_capture_begin(r->act);
      else if (edges & PSP_CTRL_SQUARE)
         ctl_apply(r->act, 0, "none");
      else if (edges & PSP_CTRL_TRIANGLE)
         ctl_apply(r->act, ctl_default(r->act), "default");
   }
   else if (r->kind == CR_MENU &&
            (edges & (PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE)))
   {
      int was = g_pcfg.controls.menu_home;
      g_pcfg.controls.menu_home = (edges & PSP_CTRL_CROSS)
         ? (uint8_t)!was : (uint8_t)CTL_MENU_START_SELECT;
      if (g_pcfg.controls.menu_home != was)
         g_settings_dirty = 1;
      snprintf(g_ctl_note, sizeof(g_ctl_note), "%s",
               g_pcfg.controls.menu_home
                  ? "In game HOME opens the menu  -  hold START+SELECT to quit"
                  : "START+SELECT (hold) opens the menu  -  HOME is the system's");
      fe_evt("ctl_menu_button value=%s",
             ctl_menu_name(g_pcfg.controls.menu_home));
   }
   else if (r->kind == CR_TURBO && (edges & PSP_CTRL_CROSS))
   {
      g_tp.on = 1;
      g_tp.row = 0;
      g_tp.col = 0;
      g_ctl_note[0] = '\0';
      fe_evt("ctl_turbo_page");
      return screen_turbo(0);
   }
   else if (r->kind == CR_RESET && (edges & PSP_CTRL_CROSS))
   {
      if (!g_ctl_reset_armed)
      {
         g_ctl_reset_armed = 1;
         snprintf(g_ctl_note, sizeof(g_ctl_note),
                  "Press " VID_GLYPH_X " again to reset every control");
      }
      else
      {
         ctl_reset(&g_pcfg.controls);
         g_ctl_reset_armed = 0;
         g_settings_dirty = 1;
         snprintf(g_ctl_note, sizeof(g_ctl_note),
                  "Every control is back to its default");
         fe_evt("ctl_reset");
      }
   }

   ov_backdrop(OV_SCRIM_DENSE);
   ov_head(NULL, "CONTROLS");
   pitch = ctl_pitch();
   for (c = 0; c < 2; c++)
   {
      int cx = CTL_COLS[c].x, cw = CTL_COLS[c].w;
      int lx = cx + vid_text_w(CTL_COLS[c].title) + FE_FONT_W;
      vid_text(cx, CTL_COL_Y, CTL_COLS[c].title, C_ACCENT);
      vid_rect(lx, CTL_COL_Y + FE_FONT_H / 2, cx + cw - 10 - lx, 1,
               C_ACCENT_DK, 120);
      for (i = 0; i < CTL_ROWS_N; i++)
      {
         const ctl_row *ri = &CTL_ROWS[i];
         int y, sel = (i == g_cursor), xr = cx + cw - 10, n, w;
         if (ri->col != c || !ctl_row_shown(i))
            continue;
         y = CTL_ROW_Y0 + ctl_row_pos(i) * pitch;
         if (sel)
         {
            int by = vid_band_y(y, pitch + 1);
            vid_rect(cx - 8, by, cw + 6, pitch + 1, C_ACCENT, 40);
            vid_rect(cx - 8, by, 3, pitch + 1, C_ACCENT, 255);
         }
         vid_text(cx + 4, y,
                  ri->kind == CR_ACT ? ctl_info(ri->act)->label :
                  ri->kind == CR_FIXED ? ctl_fixed_row(ri, NULL, NULL)
                                       : ri->label,
                  ri->kind == CR_FIXED ? C_DIM : (sel ? C_SEL : C_ITEM));
         switch (ri->kind)
         {
         case CR_FIXED:
         {
            const char *tag;
            ctl_fixed_row(ri, &parts[0], &tag);
            w = combo_right(xr, y, parts, 1, CHIP_LOCKED, 255);
            ov_text_right(xr - w - 8, y, tag, C_DIM);
            break;
         }
         case CR_MENU:
            parts[0] = g_pcfg.controls.menu_home ? "HOME" : "START+SELECT";
            w = combo_right(xr, y, parts, 1, CHIP_BOUND, 255);
            ov_text_right(xr - w - 8, y,
                          g_pcfg.controls.menu_home ? "in game" : "hold",
                          C_DIM);
            break;
         case CR_TURBO:
         {
            /* How many are set; the page behind the row has the rest. */
            char t[20];
            n = tp_bound_count();
            snprintf(t, sizeof(t), "%d set", n);
            parts[0] = t;
            combo_right(xr, y, parts, n ? 1 : 0,
                        g_turbo_locked ? CHIP_LOCKED : CHIP_BOUND, 255);
            break;
         }
         case CR_ACT:
            if (g_cap.on && g_cap.act == ri->act)
            {
               if (g_cap.phase == 2 && g_cap.got)
                  n = chord_parts(g_cap.got, parts);
               else
               {
                  parts[0] = "press...";
                  n = 1;
               }
               combo_right(xr, y, parts, n, CHIP_CAPTURE, ctl_pulse());
            }
            else
            {
               n = chord_parts(g_pcfg.controls.bind[ri->act], parts);
               combo_right(xr, y, parts, n, CHIP_BOUND, 255);
            }
            break;
         default:
            break;
         }
      }
   }
   /* An empty chip is never ambiguous: "blank = disabled" is in every
    * footer, and the plates say why a chip just went blank. */
   if (g_ctl_warn)
      ctl_warn_draw();            /* its own OK: no footer under it */
   else if (g_cap.on)
      ctl_capture_plate();
   else if (g_ctl_note[0])
      ov_plate(g_ctl_note);
   else if (r->kind == CR_TURBO && g_turbo_locked)
      ov_plate(TURBO_LOCK_MSG);
   else if (r->kind == CR_TURBO)
      footer(VID_GLYPH_X " open   " VID_GLYPH_O " back      "
             "auto-fire for A and B");
   else if (r->kind == CR_RESET)
      footer(VID_GLYPH_X " reset   " VID_GLYPH_O " back      blank = disabled");
   else if (r->kind == CR_MENU)
      footer(VID_GLYPH_X " change   " VID_GLYPH_TRI " default   "
             VID_GLYPH_O " back");
   else
      footer(VID_GLYPH_X " rebind   " VID_GLYPH_SQ " none   "
             VID_GLYPH_TRI " default   " VID_GLYPH_O " back      "
             "blank = disabled");
   return UI_ACT_NONE;
}

/* ----- wireless screens --------------------------------------------------- */
/* docs/GBA-LINK-UI.md (the design: builds/linkmode-mockups).  Three games,
 * three menus, one layout: the Host row's value is the mode it will use,
 * and the two explainer lines name the game and say the one thing the
 * player must know.  Only a game that links both ways asks (SCR_HOST). */

enum { WL_HOST, WL_SCAN, WL_JOINCODE, WL_MGIFT, WL_FAST, WL_BACK, WL_COUNT };
enum { WLS_DISCONNECT, WLS_BACK, WLS_COUNT };

/* One wireless-family row: 22 px pitch from y=120, the band 300 wide,
 * the value right-aligned at its end. */
#define WL_ROW_Y0 120
#define WL_STATUS_ROW_Y0 136     /* hosting / linked: under three lines  */
static void wl_row_at_w(int y, int w, int sel, int enabled, const char *label,
                        const char *value)
{
   if (sel)
      ov_band(y, w, 24);
   vid_text(18, y, label, enabled ? (sel ? C_SEL : C_ITEM) : C_DIM);
   if (value)
      ov_text_right(w, y, value, enabled ? C_ACCENT_DK : C_DIM);
}

static void wl_row_at(int y, int sel, int enabled, const char *label,
                      const char *value)
{
   wl_row_at_w(y, 300, sel, enabled, label, value);
}

static void wl_row(int i, int sel, const char *label, const char *value)
{
   wl_row_at(WL_ROW_Y0 + i * 22, sel, 1, label, value);
}

/* The Wireless screen's rows for this console: GB/GBC has no Mystery Gift
 * (a GBA cartridge feature).  Returns the count; ids[] gets WL_* in order. */
static int wl_rows(int ids[WL_COUNT])
{
   int n = 0;
   ids[n++] = WL_HOST;
   ids[n++] = WL_SCAN;
   ids[n++] = WL_JOINCODE;
   if (g_pcfg.console == FE_CONSOLE_GBA)
      ids[n++] = WL_MGIFT;
   /* Fast link: GBA only (it does nothing for the GB link), and above Back
    * only, so Host / Join keep the rows the rig's menu scripts count. */
   if (g_pcfg.console == FE_CONSOLE_GBA)
      ids[n++] = WL_FAST;
   ids[n++] = WL_BACK;
   return n;
}

/* "prefix<name>suffix", the name clipped so the line fits `px`. */
static void wl_line_name(char *out, size_t cap, const char *prefix,
                         const char *name, const char *suffix, int px)
{
   char nm[64];
   int room = px - vid_text_w(prefix) - vid_text_w(suffix);
   snprintf(nm, sizeof(nm), "%s", name && name[0] ? name : "this game");
   if (room < 40)
      room = 40;
   clip_title(nm, room);
   snprintf(out, cap, "%s%s%s", prefix, nm, suffix);
}

/* The two explainer lines and the Host row's value, by what Host will do. */
static void wl_explain(char *l1, size_t c1, char *l2, size_t c2,
                       const char **host_value)
{
   int ov = fe_linkmode_get(&g_lm, g_game_key);
   if (g_pcfg.console != FE_CONSOLE_GBA)
   {
      wl_line_name(l1, c1, "Link Cable game. The other PSP needs ",
                   own_game_name(), ",", 436);
      snprintf(l2, c2, "or receives it over the link.");
      *host_value = "Link Cable";
      return;
   }
   switch (ui_host_mode())
   {
   case UI_HOST_CABLE:
      wl_line_name(l1, c1, "Link Cable game: both PSPs need ",
                   own_game_name(), " open.", 436);
      snprintf(l2, c2, "%s", ov == FE_LM_CABLE
               ? "Link mode: Link Cable (set in Settings).  2 players."
               : "This game has no Wireless Adapter mode.  2 players.");
      *host_value = "Link Cable";
      break;
   case UI_HOST_ASK:
      snprintf(l1, c1, "This game links two ways. The host chooses; Join "
               "follows.");
      snprintf(l2, c2, "Both PSPs must be in the same room.");
      *host_value = "Link Cable / Wireless Adapter";
      break;
   default:
      snprintf(l1, c1, "Wireless Adapter game. Both PSPs need the same "
               "room.");
      snprintf(l2, c2, "%s", ov == FE_LM_ADAPTER
               ? "Link mode: Wireless Adapter (set in Settings)."
               : "The game decides who plays with whom.");
      *host_value = "Wireless Adapter";
      break;
   }
}

/* Join room N (the Settings room).  A game that can only enter one kind of
 * room joins that room's exact name; a game that can enter both (and every
 * GB/GBC console, whose host's name carries ITS cartridge) scans first --
 * screen_scan with g_scan_room set, which ends in scan_resolve_room: one
 * found = join it, both = ask (owner, 2026-10-03), none = not found. */
static int g_scan_room = -1;

static ui_action wl_join_room(void)
{
   char grp[9];
   int nn = room_number(g_pcfg.group), cab = join_can_cable(),
       ada = join_can_adapter(), gba = g_pcfg.console == FE_CONSOLE_GBA;
   if (nn < 0)
   {
      /* a hand-edited group: join it verbatim, as 3.1 did */
      snprintf(g_join_group, sizeof(g_join_group), "%s", g_pcfg.group);
      screen_to(SCR_MENU);
      return UI_ACT_NET_JOIN;
   }
   if (gba && cab != ada)
   {
      char code[5];
      room_code_gba(g_game_code, code);
      room_make(cab ? ROOM_CABLE_GBA : ROOM_ADAPTER, nn, code, grp,
                sizeof(grp));
      snprintf(g_join_group, sizeof(g_join_group), "%s", grp);
      screen_to(SCR_MENU);
      return UI_ACT_NET_JOIN;
   }
   g_scan_count = -1;
   screen_to(SCR_SCAN);
   g_scan_room = nn;
   return UI_ACT_NONE;
}

/* The scan Join room N asked for is in: decide (see wl_join_room). */
static ui_action scan_resolve_room(int nn, int n)
{
   char found_c[9] = "", found_a[9] = "", lbl[ROOM_LABEL_CAP];
   int i, cab = join_can_cable(), ada = join_can_adapter(),
       gba = g_pcfg.console == FE_CONSOLE_GBA;
   room_label(g_pcfg.group, lbl, sizeof(lbl));
   for (i = 0; i < n; i++)
   {
      char code[5];
      int num, k = room_parse(g_scan_groups[i], &num, code);
      if (num != nn)
         continue;
      if (gba && k == ROOM_CABLE_GBA && cab && !strcmp(code, g_game_code))
         snprintf(found_c, sizeof(found_c), "%.8s", g_scan_groups[i]);
      else if (gba && k == ROOM_ADAPTER && ada)
         snprintf(found_a, sizeof(found_a), "%.8s", g_scan_groups[i]);
      else if (!gba && k == ROOM_CABLE_GB && !found_c[0])
         snprintf(found_c, sizeof(found_c), "%.8s", g_scan_groups[i]);
      else if (!gba && k == ROOM_ADAPTER)
         snprintf(found_a, sizeof(found_a), "%.8s", g_scan_groups[i]);
   }
   /* GB/GBC: a GB room wins; an older build's GPSPnn is tried after. */
   if (!gba && found_c[0])
      found_a[0] = '\0';
   fe_evt("wl_join_room room=%d scan=%d cable=%s adapter=%s", nn, n,
          found_c[0] ? found_c : "-", found_a[0] ? found_a : "-");
   if (found_c[0] && found_a[0])
   {
      snprintf(g_ask_group[0], sizeof(g_ask_group[0]), "%s", found_c);
      snprintf(g_ask_group[1], sizeof(g_ask_group[1]), "%s", found_a);
      screen_to(SCR_JOINASK);
      return UI_ACT_NONE;
   }
   if (!found_c[0] && !found_a[0])
   {
      osd_toast("%s not found - is the host still there?", lbl);
      screen_to(SCR_WIRELESS);
      g_cursor = WL_JOINCODE;
      return UI_ACT_NONE;
   }
   snprintf(g_join_group, sizeof(g_join_group), "%s",
            found_c[0] ? found_c : found_a);
   screen_to(SCR_MENU);
   return UI_ACT_NET_JOIN;
}

/* Wireless while a session is up (or a cable host waits): the status where
 * the explainer sits, then Stop hosting / Disconnect and Back. */
static ui_action wl_session(unsigned edges, const char *session_info)
{
   int hosting = g_link.state == UI_LINK_HOSTING;
   char l2[96], l3[96];
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + WLS_COUNT - 1) % WLS_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % WLS_COUNT;
   if (edges & PSP_CTRL_CIRCLE)
      screen_to(SCR_MENU);
   if (edges & PSP_CTRL_CROSS)
   {
      screen_to(SCR_MENU);
      if (g_cursor == WLS_DISCONNECT)
         return UI_ACT_NET_DISCONNECT;
   }
   l2[0] = l3[0] = '\0';
   if (hosting)
   {
      snprintf(l2, sizeof(l2), "Hosting %s by Link Cable. Waiting for the "
               "other PSP.", g_link.room);
      wl_line_name(l3, sizeof(l3), "They open ", own_game_name(),
                   ", then Wireless > Join.", 436);
   }
   else if (g_link.state == UI_LINK_LINKED && g_link.cable)
   {
      if (g_link.player)
         snprintf(l2, sizeof(l2), "Link Cable, %s, Player %d.", g_link.room,
                  g_link.player);
      else
         snprintf(l2, sizeof(l2), "Link Cable, %s.", g_link.room);
      snprintf(l3, sizeof(l3), "The other PSP is linked. Only buttons cross "
               "the air.");
   }
   else if (g_link.state == UI_LINK_LINKED)
   {
      if (g_link.player == 1)
      {
         snprintf(l2, sizeof(l2), "Hosting %s by Wireless Adapter.",
                  g_link.room);
         snprintf(l3, sizeof(l3), "The game decides who plays with whom.");
      }
      else
      {
         snprintf(l2, sizeof(l2), "Wireless Adapter, %s.", g_link.room);
         snprintf(l3, sizeof(l3), "The other PSP is in the room.");
      }
   }
   else
      snprintf(l2, sizeof(l2), "%s",
               session_info && session_info[0] ? session_info
                                               : "session active");
   ov_backdrop(OV_SCRIM);
   ov_head(NULL, "WIRELESS");
   vid_text(22, 72, "Status", C_DIM);
   vid_text(22, 72 + FE_FONT_H + 2, l2, C_VALUE);
   if (l3[0])
      vid_text(22, 72 + 2 * (FE_FONT_H + 2), l3, C_DIM);
   wl_row_at(WL_STATUS_ROW_Y0, g_cursor == WLS_DISCONNECT, 1,
             hosting ? "Stop hosting" : "Disconnect", NULL);
   wl_row_at(WL_STATUS_ROW_Y0 + 22, g_cursor == WLS_BACK, 1, "Back", NULL);
   /* Fast link as THIS session runs it, greyed: it cannot change now, and
    * for a guest it is the host's choice, not this console's setting. */
   if (g_link.state == UI_LINK_LINKED && g_link.cable && g_link.fast >= 0)
      wl_row_at(WL_STATUS_ROW_Y0 + 44, 0, 0, "Fast link",
                g_link.fast_src == FE_GBLINK_FL_HOST
                   ? (g_link.fast ? "on (host)" : "off (host)")
                : g_link.fast_src == FE_GBLINK_FL_OLD_PEER
                   ? "off (other PSP: older version)"
                   : (g_link.fast ? "on" : "off"));
   footer(VID_GLYPH_X " select   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

static ui_action screen_wireless(unsigned edges, int session_active,
                                 const char *session_info)
{
   int ids[WL_COUNT], n = wl_rows(ids), i, id;
   char l1[96], l2[96], lbl[ROOM_LABEL_CAP];
   const char *host_value = NULL;

   if (session_active || g_link.state != UI_LINK_OFF)
      return wl_session(edges, session_info);

   if (g_cursor >= n)
      g_cursor = 0;
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + n - 1) % n;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % n;
   id = ids[g_cursor];
   if (id == WL_JOINCODE)
   {
      if (edges & PSP_CTRL_LEFT)
         settings_adjust(SET_ROOM, -1);
      if (edges & PSP_CTRL_RIGHT)
         settings_adjust(SET_ROOM, +1);
   }
   /* Fast link: left / right / X all flip it; greyed (and inert) for a game
    * that only links by Wireless Adapter. */
   if (id == WL_FAST && ui_host_mode() != UI_HOST_ADAPTER &&
       (edges & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_CROSS)))
      settings_adjust(SET_FASTLINK, +1);
   if (edges & PSP_CTRL_CIRCLE)
      screen_to(SCR_MENU);
   if (edges & PSP_CTRL_CROSS)
   {
      switch (id)
      {
      case WL_HOST:
         if (ui_host_mode() == UI_HOST_ASK)
         {
            screen_to(SCR_HOST);
            return UI_ACT_NONE;
         }
         g_host_cable = ui_host_mode() == UI_HOST_CABLE;
         snprintf(g_join_group, sizeof(g_join_group), "%s", g_pcfg.group);
         screen_to(SCR_MENU);
         return UI_ACT_NET_HOST;
      case WL_SCAN:
         g_scan_count = -1;
         g_scan_room = -1;
         screen_to(SCR_SCAN);
         return UI_ACT_NONE;
      case WL_JOINCODE:
         return wl_join_room();
      case WL_MGIFT:
         /* Navigate only.  The Mystery Gift screen starts the radio, because
          * the player has to pick WHICH saved network to join first -- see the
          * comment on MGF_NET. */
         screen_to(SCR_MGIFT);
         return UI_ACT_NONE;
      case WL_BACK:
         screen_to(SCR_MENU);
         return UI_ACT_NONE;
      }
   }

   wl_explain(l1, sizeof(l1), l2, sizeof(l2), &host_value);
   if (id == WL_FAST)
   {
      /* the two lines under the title explain the highlighted row */
      snprintf(l1, sizeof(l1), "%s", FASTLINK_HINT1);
      snprintf(l2, sizeof(l2), "%s", FASTLINK_HINT2);
   }
   ov_backdrop(OV_SCRIM);
   ov_head(NULL, "WIRELESS");
   vid_text(22, 72, l1, C_DIM);
   vid_text(22, 72 + FE_FONT_H + 2, l2, C_DIM);
   for (i = 0; i < n; i++)
   {
      switch (ids[i])
      {
      case WL_HOST:
         wl_row(i, g_cursor == i, "Host", host_value);
         break;
      case WL_SCAN:
         wl_row(i, g_cursor == i, "Join: scan for rooms", NULL);
         break;
      case WL_JOINCODE:
         wl_row(i, g_cursor == i, "Join room",
                room_label(g_pcfg.group, lbl, sizeof(lbl)));
         break;
      case WL_MGIFT:
         wl_row(i, g_cursor == i, "Mystery Gift", "phone");
         break;
      case WL_FAST:
         if (ui_host_mode() == UI_HOST_ADAPTER)
            wl_row_at(WL_ROW_Y0 + i * 22, g_cursor == i, 0, "Fast link",
                      "Link Cable only");
         else
            wl_row(i, g_cursor == i, "Fast link",
                   g_pcfg.fast_link ? "on" : "off");
         break;
      default:
         wl_row(i, g_cursor == i, "Back", NULL);
         break;
      }
   }
   if (id == WL_FAST)
      footer("DPAD change   " VID_GLYPH_O " back");
   else
      footer(VID_GLYPH_X " select   DPAD change room   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* HOST, for a game that links both ways: the one question, asked once. */
static ui_action screen_host(unsigned edges)
{
   enum { HC_CABLE, HC_ADAPTER, HC_BACK, HC_COUNT };
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + HC_COUNT - 1) % HC_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % HC_COUNT;
   if (edges & PSP_CTRL_CIRCLE)
   {
      screen_to(SCR_WIRELESS);
      return UI_ACT_NONE;
   }
   if (edges & PSP_CTRL_CROSS)
   {
      if (g_cursor == HC_BACK)
      {
         screen_to(SCR_WIRELESS);
         return UI_ACT_NONE;
      }
      g_host_cable = g_cursor == HC_CABLE;
      snprintf(g_join_group, sizeof(g_join_group), "%s", g_pcfg.group);
      screen_to(SCR_MENU);
      return UI_ACT_NET_HOST;
   }
   ov_backdrop(OV_SCRIM);
   ov_head(g_ov_title, "HOST");
   vid_text(22, 72, "How will the other PSP connect?", C_ITEM);
   /* 460 wide, not the Wireless rows' 300: "Wireless Adapter" and its note
    * do not both fit in 300 px (the design's own render overlapped them). */
   wl_row_at_w(104, 460, g_cursor == HC_CABLE, 1, "Link Cable",
               "both PSPs need this game");
   wl_row_at_w(126, 460, g_cursor == HC_ADAPTER, 1, "Wireless Adapter",
               "the game's wireless mode");
   wl_row_at_w(148, 460, g_cursor == HC_BACK, 1, "Back", NULL);
   footer(VID_GLYPH_X " host   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* Join room N found BOTH a Link Cable and a Wireless Adapter Room N: ask
 * (owner, 2026-10-03). */
static ui_action screen_joinask(unsigned edges)
{
   enum { JA_CABLE, JA_ADAPTER, JA_BACK, JA_COUNT };
   char lbl[ROOM_LABEL_CAP], q[64];
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + JA_COUNT - 1) % JA_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % JA_COUNT;
   if (edges & PSP_CTRL_CIRCLE)
   {
      screen_to(SCR_WIRELESS);
      return UI_ACT_NONE;
   }
   if (edges & PSP_CTRL_CROSS)
   {
      if (g_cursor == JA_BACK)
      {
         screen_to(SCR_WIRELESS);
         return UI_ACT_NONE;
      }
      snprintf(g_join_group, sizeof(g_join_group), "%s",
               g_ask_group[g_cursor == JA_CABLE ? 0 : 1]);
      screen_to(SCR_MENU);
      return UI_ACT_NET_JOIN;
   }
   room_label(g_ask_group[0], lbl, sizeof(lbl));
   snprintf(q, sizeof(q), "%s is open two ways. Which one?", lbl);
   ov_backdrop(OV_SCRIM);
   ov_head(g_ov_title, "JOIN");
   vid_text(22, 72, q, C_ITEM);
   wl_row_at_w(104, 460, g_cursor == JA_CABLE, 1, "Link Cable",
               own_game_name());
   wl_row_at_w(126, 460, g_cursor == JA_ADAPTER, 1, "Wireless Adapter",
               "the game's wireless mode");
   wl_row_at_w(148, 460, g_cursor == JA_BACK, 1, "Back", NULL);
   footer(VID_GLYPH_X " join   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

/* ----- Mystery Gift ------------------------------------------------------
 *
 * A separate screen from the wireless one because it is a separate radio mode
 * with a separate failure surface (no stored network profile, wrong hotspot,
 * WLAN switch off) and because the transfer has PROGRESS worth watching.  The
 * two status lines are built by the frontend (mgift_ui_line1/2) so nothing
 * about the wire protocol leaks into the menu code.
 */
/* WHY THERE IS A NETWORK ROW HERE.
 *
 * sceNetApctlConnect() takes a STORED profile index -- one of the connections
 * set up once in XMB Settings > Network Settings.  It does not scan and it does
 * not prompt.  So the player does NOT have to leave the emulator or reconnect
 * anything: we associate on demand, mid-game, when they start Mystery Gift.
 * But "the first profile that exists" is the wrong guess the moment somebody
 * has their home Wi-Fi saved as profile 1 and the phone hotspot as profile 2 --
 * we would join the house and then wait forever for a phone that is not on it.
 * Hence: pick the profile, by name, before starting. */
enum { MGF_NET, MGF_START, MGF_BACK, MGF_COUNT };

/* 0 = AUTOMATIC: select/create the open Mystery Gift hotspot profile.
 * A nonzero selection overrides this with a saved PSP connection. */
static int g_mg_conf;

int ui_mgift_conf(void) { return g_mg_conf; }

static ui_action screen_mgift(unsigned edges)
{
   const char *l1 = mgift_ui_line1();
   const char *l2 = mgift_ui_line2();
   int  running   = mgift_ui_active();
   int  nconf     = mgnet_config_count();
   static char cname[36];

   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + MGF_COUNT - 1) % MGF_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % MGF_COUNT;

   /* The profile can only be changed while stopped -- changing it under a live
    * association would mean tearing the radio down mid-transfer. */
   if (!running && g_cursor == MGF_NET)
   {
      /* 0..nconf, wrapping: 0 is Automatic and is always offered, even when the
       * console has no saved connections at all -- which is exactly the case
       * Automatic exists for. */
      if (edges & PSP_CTRL_LEFT)
         g_mg_conf = (g_mg_conf <= 0) ? nconf : g_mg_conf - 1;
      if (edges & PSP_CTRL_RIGHT)
         g_mg_conf = (g_mg_conf >= nconf) ? 0 : g_mg_conf + 1;
   }

   /* O backs out but deliberately does NOT stop the listener: a gift can be
    * arriving, and the player may well want to watch the game while it does.
    * Stopping is an explicit choice, or happens on its own when the session
    * ends. */
   if (edges & PSP_CTRL_CIRCLE)
      screen_to(SCR_WIRELESS);
   if (edges & PSP_CTRL_CROSS)
   {
      if (g_cursor == MGF_START)
      {
         /* Starting blocks for seconds while the radio associates, so leave the
          * menu open: the frontend draws its own progress frame and this screen
          * narrates the result when we come back to it. */
         return running ? UI_ACT_NET_MGIFT_STOP : UI_ACT_NET_MGIFT;
      }
      if (g_cursor == MGF_BACK)
         screen_to(SCR_WIRELESS);
   }

   ov_backdrop(OV_SCRIM);
   ov_head(NULL, "MYSTERY GIFT");
   ov_status_right(running ? "LISTENING" : "OFF");
   /* The station's two lines where the wireless explainer sits. */
   vid_text(22, 72, l1 && l1[0] ? l1 : "Station not started", C_VALUE);
   if (l2 && l2[0])
      vid_text(22, 72 + FE_FONT_H + 2, l2, C_ITEM);

   /* WHAT TO SET THE PHONE TO.  The connection profile is made for us, so the
    * only thing the player has to get right is the hotspot itself -- and the
    * one place they will be looking when it does not connect is this screen.
    * Spelling it out here beats a README they do not have on them. */
   if (!running)
   {
      vid_text(22, 196, "Phone: Mobile Hotspot named  Mystery Gift", C_DIM);
      vid_text(22, 196 + FE_FONT_H + 2, "Security: Open    Band: 2.4 GHz",
               C_DIM);
   }

   /* Name the profile, so "which network is this going to join" is answered on
    * screen instead of guessed.  No profiles at all is the one failure worth
    * spelling out in full -- it is a trip to XMB, not a retry. */
   if (g_mg_conf == 0)
      snprintf(cname, sizeof(cname), "Automatic (Mystery Gift)");
   else if (mgnet_config_name(g_mg_conf, cname, sizeof(cname)) != 0 || !cname[0])
      snprintf(cname, sizeof(cname), "connection %d", g_mg_conf);

   wl_row_at(WL_ROW_Y0, g_cursor == MGF_NET, !running, "Network", cname);
   wl_row(1, g_cursor == MGF_START,
          running ? "Stop listening" : "Start listening", NULL);
   wl_row(2, g_cursor == MGF_BACK, "Back", NULL);

   if (running)
      footer(VID_GLYPH_X " select   " VID_GLYPH_O " back (keeps listening)");
   else if (g_mg_conf == 0)
      footer("SELECT+DOWN toggles in game.  " VID_GLYPH_X " start   "
             VID_GLYPH_O " back");
   else
      footer("DPAD pick network   " VID_GLYPH_X " start   " VID_GLYPH_O
             " back");
   return UI_ACT_NONE;
}

/* The scan list: three columns, room / mode / game (docs/GBA-LINK-UI.md).
 * A row this game can join is lit; the others are dimmed but still listed,
 * so "my friend's room is there, why can't I join" is answered on the spot:
 * X on a dimmed row puts the reason on a plate where the footer was, and
 * the next d-pad press clears it.  Join room N comes through here too when
 * it must look first (g_scan_room). */
#define SCAN_X_MODE  96
#define SCAN_X_TITLE 226
#define SCAN_X_END   456

/* Harness `ui_scan_fake = GPSP07,GC07BPEE,...`: the scan answers with these
 * group names instead of the radio, so every scan-list state of the design
 * can be photographed on one console (docs/GBA-LINK-UI.md).  "-" = none. */
static char g_scan_fake[96];
void ui_scan_fake_set(const char *csv)
{
   snprintf(g_scan_fake, sizeof(g_scan_fake), "%s", csv ? csv : "");
}

static int scan_fake_fill(void)
{
   const char *p = g_scan_fake;
   int n = 0;
   if (!strcmp(g_scan_fake, "-"))
      return 0;
   while (*p && n < 8)
   {
      int k = 0;
      while (*p && *p != ',' && k < 8)
         g_scan_groups[n][k++] = *p++;
      g_scan_groups[n][k] = '\0';
      while (*p && *p != ',')
         p++;
      if (*p == ',')
         p++;
      if (k)
         n++;
   }
   return n;
}

static ui_action screen_scan(unsigned edges)
{
   int rows = (g_scan_count > 0 ? g_scan_count : 0) + 1;
   int i;

   if (g_scan_count < 0)
   {
      /* Draw one "scanning" frame; the blocking scan runs on the NEXT
       * frame so the message is visible during the wait. */
      static int drew_notice;
      ov_backdrop(OV_SCRIM);
      ov_head(NULL, "WIRELESS");
      ov_status_right("SCANNING");
      if (g_scan_room >= 0)
      {
         char lbl[ROOM_LABEL_CAP], t[48];
         snprintf(t, sizeof(t), "Looking for %s (10 s)...",
                  room_label(g_pcfg.group, lbl, sizeof(lbl)));
         vid_text(22, 72, t, C_ITEM);
      }
      else
         vid_text(22, 72, "Searching for rooms (10 s)...", C_ITEM);
      vid_rect(22, 72 + FE_FONT_H + 8, 200, 3, C_ACCENT_DK, 200);
      if (drew_notice)
      {
         int n = g_scan_fake[0] ? scan_fake_fill()
                                : adhoc_transport_scan(g_scan_groups, 8, 0);
         g_scan_count = (n < 0) ? 0 : n;
         g_scan_note[0] = '\0';
         fe_evt("wl_scan result=%d rc_stage=%s", n, adhoc_transport_stage());
         for (i = 0; i < g_scan_count; i++)
            fe_evt("wl_scan_room i=%d group=%s", i, g_scan_groups[i]);
         drew_notice = 0;
         if (n == ADHOC_ERR_WLAN_OFF)
         {
            osd_toast("WLAN switch is OFF");
            g_scan_room = -1;
         }
         if (g_scan_room >= 0)
         {
            int nn = g_scan_room;
            g_scan_room = -1;
            return scan_resolve_room(nn, g_scan_count);
         }
      }
      else
         drew_notice = 1;
      return UI_ACT_NONE;
   }

   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
      g_scan_note[0] = '\0';         /* read; the footer comes back */
   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + rows - 1) % rows;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % rows;
   if (edges & PSP_CTRL_CIRCLE)
      screen_to(SCR_WIRELESS);
   else if (edges & PSP_CTRL_CROSS)
   {
      if (g_scan_count > 0 && g_cursor < g_scan_count)
      {
         scan_row r;
         scan_row_decide(g_scan_groups[g_cursor], &r);
         if (r.ok)
         {
            snprintf(g_join_group, sizeof(g_join_group), "%s",
                     g_scan_groups[g_cursor]);
            screen_to(SCR_MENU);
            return UI_ACT_NET_JOIN;
         }
         snprintf(g_scan_note, sizeof(g_scan_note), "%s", r.plate);
         fe_evt("wl_scan_refused group=%s why=\"%s\"",
                g_scan_groups[g_cursor], r.plate);
      }
      else
         screen_to(SCR_WIRELESS);
   }
   if (g_screen != SCR_SCAN)
      return UI_ACT_NONE;

   ov_backdrop(OV_SCRIM);
   ov_head(NULL, "WIRELESS");
   ov_status_right(g_scan_count ? "ROOMS FOUND" : "NO ROOMS");
   if (g_scan_count > 0)
   {
      /* Up to eight rooms + Back from y=72: the explainer has nothing to
       * say here, so the list takes its place. */
      for (i = 0; i < g_scan_count; i++)
      {
         char lbl[ROOM_LABEL_CAP], t[64];
         scan_row r;
         int y = 72 + i * 20, sel = g_cursor == i;
         scan_row_decide(g_scan_groups[i], &r);
         if (sel)
            ov_band(y, 460, 24);
         vid_text(18, y, room_label(g_scan_groups[i], lbl, sizeof(lbl)),
                  r.ok ? (sel ? C_SEL : C_ITEM) : C_DIM);
         if (r.mode[0])
            vid_text(SCAN_X_MODE, y, r.mode, r.ok ? C_ACCENT_DK : C_DIM);
         if (r.title[0])
         {
            snprintf(t, sizeof(t), "%s", r.title);
            clip_title(t, SCAN_X_END - SCAN_X_TITLE);
            vid_text(SCAN_X_TITLE, y, t, r.ok ? C_ITEM : C_DIM);
         }
      }
      wl_row_at(72 + g_scan_count * 20, g_cursor == g_scan_count, 1, "Back",
                NULL);
   }
   else
   {
      vid_text(22, 72, "No rooms answered the scan.", C_DIM);
      vid_text(22, 72 + FE_FONT_H + 2,
               "Have the other PSP host first, then rescan.", C_DIM);
      wl_row(0, 1, "Back", NULL);
   }
   if (g_scan_note[0])
      ov_plate(g_scan_note);
   else
      footer(VID_GLYPH_X " join   " VID_GLYPH_O " back");
   return UI_ACT_NONE;
}

#ifdef GPSP_CHEATS
/* ----- cheats (docs/CHEATS.md) ---------------------------------------------
 *
 * ONE PAGE, TWO DOORS: the in-game menu's Cheats row (the running game) and
 * RIGHT in the ROM browser (the highlighted game, before it is launched).
 * Model and view are separate, as on the Controls page: what a cheat file
 * means, which codes are acceptable and which entries are on is fe_cheats.c
 * (host-tested); this section finds the file, walks rows and draws.
 *
 * THE FILES.  <app>/cheats/<rom name minus extension>.cht (or .txt, for a
 * file made in a text editor that insists on it) is the list; it is only
 * ever READ.  <same name>.on beside it is the player's choice, rewritten on
 * each change -- a few hundred bytes, like favourites.txt.
 *
 * MEMORY.  Nothing here allocates and nothing uses stdio: with a game loaded
 * there is no heap left (heap-exhausted-by-design).  The file is read with
 * sceIo straight into fe_cheats' static buffer. */
#define CHT_ROWS      10                 /* rows in the viewport            */
#define CHT_ROW_Y0    SET_VIEW_TOP
#define CHT_PITCH     FE_FONT_H
#define CHT_NAME_X    36
#define CHT_VALUE_XR  440

static char g_cht_dir[160];              /* <app>/cheats                    */
static char g_cht_stem[PSP_ROM_REL_PATH_CAP];
static char g_cht_file[PSP_FILE_PATH_CAP]; /* the file read, "" = none      */
static char g_cht_state[FE_CHEATS_STATE_MAX];
static int  g_cht_cur, g_cht_top;        /* cursor row, first visible row   */
static int  g_cht_ask;                   /* "all off?" is on the plate      */
static char g_cht_note[72];              /* the plate, until the next key   */

/* Read up to cap bytes; *more = the file is longer.  -1 = no such file. */
static int cht_read(const char *path, char *buf, int cap, int *more)
{
   SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
   int n;
   char one;
   if (more)
      *more = 0;
   if (fd < 0)
      return -1;
   n = sceIoRead(fd, buf, cap);
   if (n == cap && more && sceIoRead(fd, &one, 1) == 1)
      *more = 1;
   sceIoClose(fd);
   return n < 0 ? 0 : n;
}

static int cht_path(char *out, size_t cap, const char *ext)
{
   int n = snprintf(out, cap, "%s/%s%s", g_cht_dir, g_cht_stem, ext);
   return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* The player's choices, to <name>.on.  0 = written. */
static int cht_state_save(void)
{
   char path[PSP_FILE_PATH_CAP];
   size_t n = fe_cheats_state_serialise(g_cht_state, sizeof(g_cht_state));
   SceUID fd;
   int rc = 0;
   if (n >= sizeof(g_cht_state) || cht_path(path, sizeof(path), ".on") != 0)
      return -1;
   fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd < 0 || sceIoWrite(fd, g_cht_state, (int)n) != (int)n)
      rc = -1;
   if (fd >= 0)
      sceIoClose(fd);
   fe_evt("cheats_save on=%u rc=%d", fe_cheats_enabled_count(), rc);
   return rc;
}

int ui_cheats_load(const char *rom_path, int console, int attach)
{
   extern char g_dir_base[];
   const char *b;
   char *dot;
   int n = -1, more = 0, sn = 0;
   int sys = console == FE_CONSOLE_GBA ? FE_CHEATS_SYS_GBA : FE_CHEATS_SYS_GB;
   unsigned t0 = sceKernelGetSystemTimeLow();
   FE_EVT_ONLY(t0);

   fe_cheats_reset();
   g_cht_file[0] = '\0';
   g_cht_stem[0] = '\0';
   if (!rom_path)
      return 0;
   snprintf(g_cht_dir, sizeof(g_cht_dir), "%s/cheats", g_dir_base);
   b = strrchr(rom_path, '/');
   snprintf(g_cht_stem, sizeof(g_cht_stem), "%s", b ? b + 1 : rom_path);
   dot = strrchr(g_cht_stem, '.');
   if (dot && dot != g_cht_stem)
      *dot = '\0';

   if (cht_path(g_cht_file, sizeof(g_cht_file), ".cht") == 0)
      n = cht_read(g_cht_file, fe_cheats_buffer(), FE_CHEATS_FILE_MAX, &more);
   if (n < 0 && cht_path(g_cht_file, sizeof(g_cht_file), ".txt") == 0)
      n = cht_read(g_cht_file, fe_cheats_buffer(), FE_CHEATS_FILE_MAX, &more);
   if (n < 0)
      g_cht_file[0] = '\0';
   else
   {
      char path[PSP_FILE_PATH_CAP];
      fe_cheats_parse((size_t)n, sys, more);
      if (cht_path(path, sizeof(path), ".on") == 0 &&
          (sn = cht_read(path, g_cht_state, sizeof(g_cht_state), NULL)) > 0)
         fe_cheats_state_parse(g_cht_state, (size_t)sn);
   }
   fe_cheats_attach(attach);
   fe_evt("cheats_load game=\"%s\" sys=%d file=%d bytes=%d trunc=%d listed=%u "
          "found=%u usable=%u on=%u attach=%d us=%u", g_cht_stem, sys,
          n >= 0, n, more, fe_cheats_count(), fe_cheats_found(),
          fe_cheats_usable_count(), fe_cheats_enabled_count(), attach,
          (unsigned)(sceKernelGetSystemTimeLow() - t0));
   return (int)fe_cheats_enabled_count();
}

void ui_cheats_unload(void)
{
   fe_cheats_reset();
   g_cht_file[0] = '\0';
}

static void cht_page_reset(void)
{
   g_cht_cur = g_cht_top = 0;
   g_cht_ask = 0;
   g_cht_note[0] = '\0';
   /* A list that was cut says so as the page opens (the browser's lesson:
    * a cap that is hit silently reads as "broken", not "full"). */
   if (fe_cheats_found() > fe_cheats_count())
      snprintf(g_cht_note, sizeof(g_cht_note), "Showing %u of %u cheats",
               fe_cheats_count(), fe_cheats_found());
   else if (fe_cheats_truncated())
      snprintf(g_cht_note, sizeof(g_cht_note),
               "The file is over %d KB: showing the first %u cheats",
               FE_CHEATS_FILE_MAX / 1024, fe_cheats_count());
}

/* The words the Cheats row of the in-game menu carries. */
static const char *cht_menu_value(char *out, size_t cap)
{
   unsigned on = fe_cheats_enabled_count();
   if (fe_cheats_status() != FE_CHEATS_LOADED)
      return "none for this game";
   if (fe_cheats_locked())
      return on ? "paused for link play" : "off for link play";
   if (!on)
      return "all off";
   snprintf(out, cap, "%u on", on);
   return out;
}

static void cht_row(int y, int sel, const fe_cheat *e, int locked)
{
   char t[96];
   const char *val;
   int usable = !e->error, live = usable && !locked;
   uint16_t cn, cv;
   int vw;

   if (sel)
      ov_band(y, SET_BAND_W, SET_BAND_H);
   if (e->error)
   {
      val = fe_cheats_error_name(e->error);
      cn = cv = C_DIM;
   }
   else if (e->enabled)
   {
      val = "ON";
      cn = live ? C_SEL : C_DIM;
      cv = live ? C_CON : C_DIM;
   }
   else
   {
      val = "off";
      cn = live ? (sel ? C_SEL : C_ITEM) : C_DIM;
      cv = live ? C_ACCENT_DK : C_DIM;
   }
   /* The marker: what is on reads down the left edge without reading a
    * word.  Filled in the console colour = on; an outline = off; nothing =
    * a cheat this build cannot run. */
   if (usable)
   {
      int my = vid_band_y(y, 8);
      if (e->enabled)
         vid_rect(22, my, 8, 8, live ? C_CON : C_DIM, 255);
      else
         ov_outline(22, my, 8, 8, C_DIM, 160);
   }
   vw = vid_text_w(val);
   snprintf(t, sizeof(t), "%s", e->name);
   clip_title(t, CHT_VALUE_XR - vw - 14 - CHT_NAME_X);
   vid_text(CHT_NAME_X, y, t, cn);
   ov_text_right(CHT_VALUE_XR, y, val, cv);
}

/* No list to show: say where a cheat file goes, with this game's own file
 * name, so it can be typed off the screen. */
static void cht_empty(void)
{
   char t[PSP_FILE_PATH_CAP + 8];
   int have_file = g_cht_file[0] != '\0';
   const char *head = have_file ? "No cheats in this game's file"
                                : "No cheats for this game yet";
   vid_text_hd((VID_SCR_W - vid_text_hd_w(head)) / 2, 84, head, C_SEL);
   if (have_file)
   {
      vid_text_center(118, "Nothing in it reads as a cheat:", C_ITEM);
      snprintf(t, sizeof(t), "%s", g_cht_file);
   }
   else
   {
      vid_text_center(118, "Put a cheat file on the Memory Stick:", C_ITEM);
      snprintf(t, sizeof(t), "%s/", g_cht_dir);
   }
   if (vid_text_w(t) > 440)
      snprintf(t, sizeof(t), "%s", have_file ? "cheats/" : "cheats/ in the GBAdhoc folder");
   vid_text_center(140, t, C_VALUE);
   if (!have_file)
   {
      snprintf(t, sizeof(t), "%s.cht", g_cht_stem);
      clip_title(t, 440);
      vid_text_center(158, t, C_VALUE);
   }
   vid_text_center(184, "A text file: a name on one line, its codes under it.",
                   C_DIM);
   vid_text_center(202, fe_cheats_system() == FE_CHEATS_SYS_GBA
                           ? "GBA: CodeBreaker codes, e.g. 82025BC4 E0FF"
                           : "GB / GBC: GameShark codes, e.g. 010AD5C1",
                   C_DIM);
}

/* One frame of the page.  Returns 1 when the player left it (O). */
static int screen_cheats(unsigned edges, const char *game)
{
   int n = (int)fe_cheats_count(), i, locked = fe_cheats_locked();
   int leave;
   char st[40];

   if (n > 0)
   {
      if (g_cht_ask)
      {
         /* The question holds the page: X answers it, any other key keeps
          * the cheats (and is not acted on, as on the Save states page). */
         if (edges & PSP_CTRL_CROSS)
         {
            fe_cheats_all_off();
            snprintf(g_cht_note, sizeof(g_cht_note),
                     cht_state_save() == 0 ? "All cheats are off"
                                           : "All off (could not save that)");
            fe_evt("cheats_all_off");
         }
         if (edges)
            g_cht_ask = 0;
         edges = 0;
      }
      if (edges)
         g_cht_note[0] = '\0';
      if (edges & PSP_CTRL_UP)
         g_cht_cur = (g_cht_cur + n - 1) % n;
      if (edges & PSP_CTRL_DOWN)
         g_cht_cur = (g_cht_cur + 1) % n;
      if (edges & PSP_CTRL_LTRIGGER)
         g_cht_cur = g_cht_cur >= CHT_ROWS ? g_cht_cur - CHT_ROWS : 0;
      if (edges & PSP_CTRL_RTRIGGER)
         g_cht_cur = g_cht_cur + CHT_ROWS < n ? g_cht_cur + CHT_ROWS : n - 1;
      if (edges & PSP_CTRL_CROSS)
      {
         const fe_cheat *e = fe_cheats_get((unsigned)g_cht_cur);
         if (locked)
            ;                    /* the plate below already says why */
         else if (e->error)
            snprintf(g_cht_note, sizeof(g_cht_note), "%s",
                     e->error == 6 /* CheatErrorGameShark */
                        ? "Choose a format: GS1:, AR3:, GS1RAW: or AR3RAW:"
                     : e->error == 7 /* CheatErrorRegion */ ||
                       e->error == FE_CHEAT_ERR_GB_REGION
                        ? "Only codes that write the game's RAM can be used"
                     : e->error == FE_CHEAT_ERR_GB_GENIE
                        ? "Game Genie codes patch the ROM: cannot be used"
                     : e->error == FE_CHEAT_ERR_GB_TYPE
                        ? "Only GameShark codes that start 01 can be used"
                        : "This code is not one GBAdhoc can read");
         else
         {
            int want = !e->enabled;
            int err = fe_cheats_set((unsigned)g_cht_cur, want);
            int rc = cht_state_save();
            if (err)
               snprintf(g_cht_note, sizeof(g_cht_note),
                        "Too many cheats on at once: switch one off first");
            else if (rc != 0)
               snprintf(g_cht_note, sizeof(g_cht_note),
                        "Changed, but the choice could not be saved");
            fe_evt("cheats_set i=%d on=%d err=%d now_on=%u hash=%08lx",
                   g_cht_cur, want, err, fe_cheats_enabled_count(),
                   (unsigned long)e->hash);
         }
      }
      if ((edges & PSP_CTRL_SQUARE) && !locked && fe_cheats_enabled_count())
         g_cht_ask = 1;
   }
   /* O leaves -- after this frame is drawn, so the page never blinks. */
   leave = (edges & PSP_CTRL_CIRCLE) != 0;

   /* keep the cursor in the viewport */
   if (g_cht_cur < g_cht_top)
      g_cht_top = g_cht_cur;
   if (g_cht_cur >= g_cht_top + CHT_ROWS)
      g_cht_top = g_cht_cur - CHT_ROWS + 1;
   if (g_cht_top > n - CHT_ROWS)
      g_cht_top = n > CHT_ROWS ? n - CHT_ROWS : 0;

   ov_backdrop(OV_SCRIM_DENSE);
   if (n <= 0)
   {
      ov_head(game, "CHEATS");
      cht_empty();
      footer(VID_GLYPH_O " back");
      return leave;
   }
   /* Line 2: the game's name, clipped short of the count ("3 of 12 on") and
    * of the room when a session has put it there.  Locked, the plate says
    * why, so the count gives its place back to the name. */
   ov_head(NULL, "CHEATS");
   {
      char t[100];
      int right = g_ov_room2_w ? 460 - g_ov_room2_w - 14 : 460;
      st[0] = '\0';
      if (!locked)
      {
         snprintf(st, sizeof(st), "%u of %d on", fe_cheats_enabled_count(),
                  n);
         ov_status_right(st);
         right -= vid_text_w(st) + 14;
      }
      snprintf(t, sizeof(t), "%.99s", game ? game : "");
      clip_title(t, right - 22);
      vid_text(22, 48, t, C_DIM);
   }
   for (i = g_cht_top; i < n && i < g_cht_top + CHT_ROWS; i++)
      cht_row(CHT_ROW_Y0 + (i - g_cht_top) * CHT_PITCH, i == g_cht_cur,
              fe_cheats_get((unsigned)i), locked);
   ov_scrollbar(470, CHT_ROW_Y0, CHT_ROWS * CHT_PITCH, g_cht_top * CHT_PITCH,
                CHT_ROWS * CHT_PITCH, n * CHT_PITCH);
   if (locked)
      ov_plate("Cheats are off during link play. They come back when it ends.");
   else if (g_cht_ask)
   {
      char q[72];
      unsigned on = fe_cheats_enabled_count();
      if (on == 1)
         snprintf(q, sizeof(q), VID_GLYPH_X " switch it off     "
                  VID_GLYPH_O " keep it on");
      else
         snprintf(q, sizeof(q), VID_GLYPH_X " switch all %u off     "
                  VID_GLYPH_O " keep them on", on);
      ov_plate(q);
   }
   else if (g_cht_note[0])
      ov_plate(g_cht_note);
   else
      footer(fe_cheats_enabled_count()
                ? VID_GLYPH_X " on/off     " VID_GLYPH_SQ " all off     "
                  VID_GLYPH_O " back"
                : VID_GLYPH_X " on/off     " VID_GLYPH_O " back");
   return leave;
}
#endif /* GPSP_CHEATS */

/* ----- main menu ---------------------------------------------------------- */

/* M_GAMELIST sits after Settings so the demo script's fixed row counts for
 * rows 0-3 stay valid (it never navigates past Settings).  Save and load are
 * one row and one page since 3.5 (docs/UI-OVERLAY.md section 13). */
/* Cheats (docs/CHEATS.md) sits after Settings for the same reason: rows 0-3
 * keep their positions; quit_demo_script takes one more DOWN. */
enum { M_RESUME, M_SAVESTATES, M_WIRELESS, M_SETTINGS,
#ifdef GPSP_CHEATS
       M_CHEATS,
#endif
       M_GAMELIST, M_EXIT, M_COUNT };

static ui_action screen_menu(unsigned edges, int session_active)
{
   static const char *labels[M_COUNT] = {
      "Resume", "Save states", "Wireless", "Settings",
#ifdef GPSP_CHEATS
      "Cheats",
#endif
      "Quit to game list", "Exit"
   };
   int i, k, fade, dy;

   if (edges & PSP_CTRL_UP)
      g_cursor = (g_cursor + M_COUNT - 1) % M_COUNT;
   if (edges & PSP_CTRL_DOWN)
      g_cursor = (g_cursor + 1) % M_COUNT;
   if (edges & PSP_CTRL_CIRCLE)
      return UI_ACT_RESUME;
   if (edges & PSP_CTRL_CROSS)
   {
      switch (g_cursor)
      {
      case M_RESUME:    return UI_ACT_RESUME;
      case M_SAVESTATES:
         if (session_active)
         {
            osd_toast("Savestates locked during wireless session");
            break;
         }
         g_state_slot = 1;
         screen_to(SCR_STATE_SLOTS);
         return screen_state_slots(0);   /* draw this frame as the new page */
      case M_WIRELESS:  screen_to(SCR_WIRELESS); break;
      case M_SETTINGS:  screen_to(SCR_SETTINGS); break;
#ifdef GPSP_CHEATS
      case M_CHEATS:
         /* Open during a link session too: the page then says why every
          * cheat is off, which a locked row could not. */
         screen_to(SCR_CHEATS);
         screen_cheats(0, g_ov_title);   /* draw this frame as the new page */
         return UI_ACT_NONE;
#endif
      case M_GAMELIST:  return UI_ACT_GAMELIST;
      case M_EXIT:      return UI_ACT_EXIT;
      }
   }

   /* THE OPENING: over OV_OPEN_FRAMES frames the scrim comes up from 0 to
    * 170 and the rows rise 8 px into place, their colour walked out of the
    * background (the GE has no text alpha -- see mix565).  The selection
    * band lands on the last frame. */
   if (g_ov_open_k < OV_OPEN_FRAMES)
      g_ov_open_k++;
   k    = g_ov_open_k;
   fade = 256 * (OV_OPEN_FRAMES - k) / OV_OPEN_FRAMES;
   dy   = 8 * (OV_OPEN_FRAMES - k) / OV_OPEN_FRAMES;

   ov_backdrop(OV_SCRIM * k / OV_OPEN_FRAMES);
   ov_head(g_ov_title, NULL);
   for (i = 0; i < M_COUNT; i++)
   {
      int y = 88 + i * 22 + dy;
      int enabled = !(i == M_SAVESTATES && session_active);
      uint16_t c = !enabled ? C_DIM : (i == g_cursor ? C_SEL : C_ITEM);
      if (i == g_cursor && k >= OV_OPEN_FRAMES)
         ov_band(y, vid_text_w(labels[i]) + 44, 24);
      vid_text(18, y, labels[i], mix565(c, C_BG_BOT, fade));
      /* The Wireless row carries the session's mode where it used to
       * say "linked" (docs/GBA-LINK-UI.md); a cable host waiting says so. */
      if (i == M_WIRELESS && (session_active || g_link.state))
         vid_text(18 + vid_text_w(labels[i]) + 12, y,
                  g_link.state == UI_LINK_HOSTING ? "hosting" :
                  g_link.state == UI_LINK_LINKED
                     ? (g_link.cable ? "Link Cable" : "Wireless Adapter")
                     : "linked",
                  mix565(C_ACCENT_DK, C_BG_BOT, fade));
      /* Save states: how many slots hold a state (the open-time stat). */
      if (i == M_SAVESTATES && enabled)
      {
         char used[24];
         snprintf(used, sizeof(used), "%d of %d used", g_ov_used,
                  PSP_STATE_SLOT_COUNT);
         vid_text(18 + vid_text_w(labels[i]) + 12, y, used,
                  mix565(C_ACCENT_DK, C_BG_BOT, fade));
      }
#ifdef GPSP_CHEATS
      /* Cheats: how many are on, or why none can be. */
      if (i == M_CHEATS)
      {
         char cv[24];
         vid_text(18 + vid_text_w(labels[i]) + 12, y,
                  cht_menu_value(cv, sizeof(cv)),
                  mix565(C_ACCENT_DK, C_BG_BOT, fade));
      }
#endif
   }
   footer(VID_GLYPH_X " select     " VID_GLYPH_O " resume");
   return UI_ACT_NONE;
}

/* DELETE A SAVE STATE (docs/CONTROL-REMAP.md section 11).  The three names
 * come from psp_state_delete_paths(), which can only ever produce the slot's
 * .stN, its .stN.thumb and its .stN.shot -- never the ROM, the .sav or
 * another slot -- and the previews go only once the state itself is gone (or
 * was never there), so a refused remove cannot leave a state without its
 * picture.  0 = the slot is now empty. */
static int state_delete(const char *slot1_path, unsigned slot)
{
   char st[PSP_FILE_PATH_CAP], th[PSP_STATE_THUMB_PATH_CAP];
   char sh[PSP_STATE_SHOT_PATH_CAP];
   SceIoStat sst;
   int rc;
   if (psp_state_delete_paths(st, sizeof(st), th, sizeof(th), sh, sizeof(sh),
                              slot1_path, slot) != 0)
   {
      fe_evt("state_delete slot=%u rc=bad_path", slot);
      return -1;
   }
   /* A leftover replacement backup must not resurrect a deleted slot. */
   {
      char old[PSP_FILE_PATH_CAP + 4];
      snprintf(old, sizeof(old), "%s.old", st);
      (void)sceIoRemove(old);
      if (sceIoGetstat(old, &sst) >= 0)
         return -1;
   }
   /* Success is judged by the file being GONE, not by the return code:
    * PPSSPP returns 0 from sceIoRemove whenever the file existed, removed
    * or not (its case-folding retry can miss a FAT short name such as
    * FIRERED.st0), and a report that says "deleted" over a state still on
    * the stick would be worse than no feature. */
   rc = sceIoRemove(st);
   if (sceIoGetstat(st, &sst) >= 0)
   {
      FE_EVT_ONLY(rc);
      fe_evt("state_delete slot=%u rc=0x%08X", slot, (unsigned)rc);
      return -1;
   }
   (void)sceIoRemove(th);      /* an "old" state never had one */
   (void)sceIoRemove(sh);      /* nor a 3.1 one this */
   fe_evt("state_delete slot=%u file=%s", slot, st);
   return 0;
}

/* One state's preview into slot i's texture: the compact .thumb the save
 * path writes beside it (state_slots.h), validated exactly as the browser's
 * state shelf validates it.  0 = a preview is there. */
static int state_thumb_read(const char *state_path, uint16_t *dst)
{
   char thumb_path[PSP_STATE_THUMB_PATH_CAP];
   unsigned char hdr[PSP_STATE_THUMB_HEADER_SIZE];
   size_t bytes = (size_t)PSP_STATE_THUMB_WIDTH * PSP_STATE_THUMB_HEIGHT * 2;
   SceUID fd;
   int ok;
   if (psp_state_thumb_path(thumb_path, sizeof(thumb_path), state_path) != 0)
      return -1;
   fd = sceIoOpen(thumb_path, PSP_O_RDONLY, 0);
   if (fd < 0)
      return -1;
   ok = sceIoRead(fd, hdr, sizeof(hdr)) == sizeof(hdr) &&
        memcmp(hdr, PSP_STATE_THUMB_MAGIC, 4) == 0 &&
        hdr[4] == PSP_STATE_THUMB_WIDTH && hdr[5] == 0 &&
        hdr[6] == PSP_STATE_THUMB_HEIGHT && hdr[7] == 0 &&
        hdr[8] == PSP_STATE_THUMB_WIDTH && hdr[9] == 0 &&
        sceIoRead(fd, dst, bytes) == (int)bytes;
   sceIoClose(fd);
   return ok ? 0 : -1;
}

/* "3 Oct 2026, 14:32" from a stat's st_mtime: the six fields as the kernel
 * stamped them from the console's own clock (the RTC quirks are time()'s,
 * memory rtc-wallclock-psp; no libc time conversion is involved).  "" when
 * the fields are not a date. */
static void ov_stamp(char *out, size_t n, const ScePspDateTime *t)
{
   static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
   if (!t || t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
       t->hour > 23 || t->minute > 59)
   {
      out[0] = '\0';
      return;
   }
   snprintf(out, n, "%d %s %d, %02d:%02d", t->day, mon[t->month - 1],
            t->year, t->hour, t->minute);
}

/* Which slots hold a state, and when each was saved: five stats.  Once per
 * menu open and once per page scan -- never per frame. */
static void ov_slots_stat_io(void)
{
   char path[PSP_FILE_PATH_CAP];
   SceIoStat st;
   int i;
   g_ov_used = 0;
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
   {
      g_ov_slot[i] = 0;
      g_ov_stamp[i][0] = '\0';
      if (!g_state_base[0] ||
          psp_state_path_for_slot(path, sizeof(path), g_state_base,
                                  (unsigned)i + 1) != 0 ||
          fe_host_state_recover(path) < 0 ||
          sceIoGetstat(path, &st) < 0)
         continue;
      g_ov_slot[i] = 1;
      g_ov_used++;
      ov_stamp(g_ov_stamp[i], sizeof(g_ov_stamp[i]), &st.sce_st_mtime);
   }
}

static void ov_slots_stat(void)
{
   pwr_hold("ss_read");
   ov_slots_stat_io();
   pwr_release();
}

/* Once, as the slots screen opens (and after a save or a delete on it):
 * which slots hold a state, their times, and their previews into the
 * thumbnail textures.  Five stats and at most five 5 KiB reads, with the
 * core paused -- never inside the draw loop.  The full preview is forgotten:
 * the slot it showed may have just changed, and it is read again on dwell. */
static void ov_slots_scan_io(void)
{
   char path[PSP_FILE_PATH_CAP];
   int i;
   ov_slots_stat();
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
   {
      g_ov_shot_none[i] = 0;
      if (g_ov_slot[i] && g_ov_thumbs &&
          psp_state_path_for_slot(path, sizeof(path), g_state_base,
                                  (unsigned)i + 1) == 0 &&
          state_thumb_read(path, g_ov_thumbs + (size_t)i *
                           PSP_STATE_THUMB_WIDTH *
                           PSP_STATE_THUMB_TEX_HEIGHT) == 0)
         g_ov_slot[i] = 2;
      /* What the page will draw for the slot (0 none, 1 state, 2 state +
       * thumbnail) and its time: the GE model's input (tools/ui_model). */
      fe_evt("ss_slot n=%d have=%d stamp=%s", i + 1, g_ov_slot[i],
             g_ov_stamp[i]);
   }
   if (g_ov_thumbs)
      sceKernelDcacheWritebackRange(g_ov_thumbs, PSP_STATE_SLOT_COUNT *
                                    PSP_STATE_THUMB_WIDTH *
                                    PSP_STATE_THUMB_TEX_HEIGHT * 2);
   g_ov_shot_slot = -1;
   g_ov_dwell = 0;
   g_slots_dirty = 0;
}

static void ov_slots_scan(void)
{
   pwr_hold("ss_read");
   ov_slots_scan_io();
   pwr_release();
}

/* Slot i's .shot into the VRAM texture: open, the 16-byte header checked
 * (state_slots.h psp_state_shot_check), then h rows of 256 texels in 4 KiB
 * reads through a stack buffer, copied in through the uncached alias.  One
 * open, at most 82 KiB, one close; nothing allocated.  0 = the texture now
 * holds slot i (g_ov_shot_slot); -1 = it holds nothing usable. */
static int ov_shot_read_io(int i)
{
   char path[PSP_FILE_PATH_CAP], shot[PSP_STATE_SHOT_PATH_CAP];
   unsigned char hdr[PSP_STATE_THUMB_HEADER_SIZE];
   uint16_t buf[8 * PSP_STATE_SHOT_STRIDE];             /* 4 KiB, 8 rows */
   unsigned w, h, y;
   SceUID fd;
   int ok;
   g_ov_shot_slot = -1;
   if (i < 0 || i >= PSP_STATE_SLOT_COUNT || !vid_shot_tex() ||
       !g_state_base[0] ||
       psp_state_path_for_slot(path, sizeof(path), g_state_base,
                               (unsigned)i + 1) != 0 ||
       psp_state_shot_path(shot, sizeof(shot), path) != 0)
      return -1;
   fd = sceIoOpen(shot, PSP_O_RDONLY, 0);
   if (fd < 0)
      return -1;
   ok = sceIoRead(fd, hdr, sizeof(hdr)) == (int)sizeof(hdr) &&
        psp_state_shot_check(hdr, &w, &h) == 0;
   for (y = 0; ok && y < h; y += 8)
   {
      unsigned n = h - y < 8 ? h - y : 8;
      int bytes = (int)(n * PSP_STATE_SHOT_STRIDE * sizeof(uint16_t));
      ok = sceIoRead(fd, buf, (SceSize)bytes) == bytes;
      if (ok)
         vid_shot_rows((int)y, buf, (int)n);
   }
   sceIoClose(fd);
   if (!ok)
      return -1;
   g_ov_shot_slot = i;
   g_ov_shot_w = (int)w;
   g_ov_shot_h = (int)h;
   return 0;
}

static int ov_shot_read(int i)
{
   int rc;
   pwr_hold("ss_read");
   rc = ov_shot_read_io(i);
   pwr_release();
   return rc;
}

/* Harness `shot_probe = 1` (main_psp.c): the read priced once, four times in
 * a row on the first slot holding a .shot, as the page first opens -- one
 * EVT line each, so hardware can confirm OV_SHOT_DWELL's premise that the
 * read fits in a frame or two.  Then forgotten: the dwell reads as usual. */
void ui_shot_probe_arm(int on)
{
   g_shot_probe = on ? 1 : 0;
}

static void ov_shot_probe(void)
{
   int i, n;
   if (!g_shot_probe)
      return;
   g_shot_probe = 0;
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
      if (g_ov_slot[i])
         break;
   for (n = 1; n <= 4 && i < PSP_STATE_SLOT_COUNT; n++)
   {
      unsigned t0 = sceKernelGetSystemTimeLow();
      int rc = ov_shot_read(i);
      FE_EVT_ONLY(t0);
      FE_EVT_ONLY(rc);
      fe_evt("shot_read n=%d us=%u rc=%d slot=%d w=%d h=%d", n,
             (unsigned)(sceKernelGetSystemTimeLow() - t0), rc, i + 1,
             rc == 0 ? g_ov_shot_w : 0, rc == 0 ? g_ov_shot_h : 0);
   }
   g_ov_shot_slot = -1;
}

/* The main loop's answer to UI_ACT_SAVESTATE / UI_ACT_LOADSTATE.  A save
 * keeps the page open and says so on its plate (the next frame rescans: one
 * stat and one thumb read; the picture comes back on dwell); a load closes
 * the menu, back into the game -- or, if it failed, stays and says so.
 * Return 1 when the page reported it (no toast wanted). */
int ui_state_save_done(int rc)
{
   int s = g_slot_saving;
   g_slot_saving = 0;
   if (!g_active || g_screen != SCR_STATE_SLOTS || !s)
      return 0;
   snprintf(g_slot_note, sizeof(g_slot_note),
            rc == 0 ? "Slot %d saved" : "Slot %d could not be saved", s);
   g_slots_dirty = 1;
   return 1;
}

int ui_state_load_done(int rc)
{
   if (!g_active || g_screen != SCR_STATE_SLOTS)
      return 0;
   if (rc == 0)
   {
      ui_close();
      return 0;                  /* the toast says it, over the game */
   }
   snprintf(g_slot_note, sizeof(g_slot_note), "Slot %d could not be loaded",
            g_state_slot);
   return 1;
}

/* THE SAVE STATES PAGE (docs/UI-OVERLAY.md section 13; Fable's direction A,
 * "Split", builds/savestate-mockups/README.md section 6).  One page for save
 * and load: the five slots on the left (the 64x42 thumbnail, the slot, its
 * time), the highlighted slot's picture 1:1 on the right.
 *
 *   X  load at once and back into the game     (empty: says so, plate)
 *   [] save; over a saved slot the plate asks first; the page stays open
 *   /\ delete, after the plate's question        (empty: nothing)
 *   O  back to the menu
 *
 * Only load is immediate, and it sits on the key that always meant "go"
 * here.  Both destructive actions have their own key AND a question, and
 * while a question is up X answers it and every other key keeps the slot,
 * so an X meant for "overwrite" or "delete" can never load instead. */
#define SS_ROW_Y(i)  (72 + 34 * (i))
#define SS_PX 228                /* the preview panel */
#define SS_PY 62
#define SS_PW 240
#define SS_PH 160

static void ss_panel(int i)
{
   const int x = SS_PX, y = SS_PY, w = SS_PW, h = SS_PH;
   const uint16_t *th = g_ov_thumbs ? g_ov_thumbs + (size_t)i *
                        PSP_STATE_THUMB_WIDTH * PSP_STATE_THUMB_TEX_HEIGHT
                                    : NULL;
   vid_rect(x + 3, y + 3, w, h, C_SHADOW, 90);
   if (!g_ov_slot[i])
   {
      static const char l1[] = "Nothing saved here";
      static const char l2[] = VID_GLYPH_SQ " save";
      vid_rect(x, y, w, h, C_CARD, 230);
      ov_outline(x, y, w, h, C_ACCENT_DK, 120);
      vid_text(x + (w - vid_text_w(l1)) / 2, y + h / 2 - 18, l1, C_DIM);
      vid_text(x + (w - vid_text_w(l2)) / 2, y + h / 2 + 2, l2, C_ITEM);
      return;
   }
   if (g_ov_shot_slot == i)
   {
      /* 1:1, nearest: the frame exactly as the player saw it.  GB/GBC
       * (160x144) sits centred on a plate. */
      int sw = g_ov_shot_w, sh = g_ov_shot_h;
      if (sw != w || sh != h)
         vid_rect(x, y, w, h, C_CARD, 255);
      vid_shot_draw(x + (w - sw) / 2, y + (h - sh) / 2, sw, sh);
      ov_outline(x, y, w, h, C_ACCENT_DK, 120);
      return;
   }
   /* No full picture (yet): the 64x42 thumbnail at 2x, nearest -- while the
    * read has not landed, for a 3.1 state for good (captioned), and always
    * when VRAM had no room for the texture. */
   vid_rect(x, y, w, h, C_CARD, 255);
   ov_outline(x, y, w, h, C_ACCENT_DK, 120);
   if (g_ov_slot[i] == 2 && th)
      vid_image_nearest(x + (w - 128) / 2, y + (h - 84) / 2 - 6, 128, 84, th,
                        PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_TEX_HEIGHT,
                        PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_HEIGHT);
   else
   {
      static const char np[] = "No preview";
      vid_text(x + (w - vid_text_w(np)) / 2, vid_text_y_in(y, h) - 8, np,
               C_DIM);
   }
   if (g_ov_shot_none[i])
   {
      static const char cap[] = "saved by 3.1: small preview only";
      vid_text(x + (w - vid_text_w(cap)) / 2, y + h - 22, cap, C_DIM);
   }
}

static ui_action screen_state_slots(unsigned edges)
{
   ui_action act = UI_ACT_NONE;
   int i, cur;

   if (g_slots_dirty)
      ov_slots_scan();           /* after a save: the slot's new files */

   /* A question on the plate (overwrite or delete): X answers it, any other
    * key keeps the slot, and no other key acts this frame. */
   if (g_slot_ask)
   {
      if (edges & PSP_CTRL_CROSS)
      {
         int s = g_slot_ask_n, ask = g_slot_ask;
         g_slot_ask = 0;
         if (ask == SLOT_ASK_DELETE)
         {
            if (state_delete(g_state_base, (unsigned)s) == 0)
               snprintf(g_slot_note, sizeof(g_slot_note), "Slot %d deleted",
                        s);
            else
               snprintf(g_slot_note, sizeof(g_slot_note),
                        "Slot %d could not be deleted", s);
            ov_slots_scan();
         }
         else
         {
            g_slot_saving = s;
            act = UI_ACT_SAVESTATE;
         }
      }
      else if (edges)
         g_slot_ask = 0;
      edges = 0;
   }
   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_CIRCLE |
                PSP_CTRL_CROSS | PSP_CTRL_SQUARE | PSP_CTRL_TRIANGLE))
      g_slot_note[0] = '\0';

   if (edges & PSP_CTRL_CIRCLE)
   {
      screen_to(SCR_MENU);
      return screen_menu(0, g_ov_linked);
   }
   if (edges & (PSP_CTRL_UP | PSP_CTRL_DOWN))
   {
      if (edges & PSP_CTRL_UP)
         g_state_slot = g_state_slot > 1 ? g_state_slot - 1
                                          : PSP_STATE_SLOT_COUNT;
      else
         g_state_slot = g_state_slot < PSP_STATE_SLOT_COUNT
                           ? g_state_slot + 1 : 1;
      g_ov_dwell = 0;
   }
   cur = g_state_slot - 1;
   if (g_state_base[0])
   {
      if (edges & PSP_CTRL_CROSS)
      {
         if (g_ov_slot[cur])
            act = UI_ACT_LOADSTATE;
         else            /* on the plate, as every page message: a toast
                          * would sit on the header */
            snprintf(g_slot_note, sizeof(g_slot_note),
                     "Slot %d is empty", g_state_slot);
      }
      else if (edges & PSP_CTRL_SQUARE)
      {
         if (g_ov_slot[cur])
         {
            g_slot_ask = SLOT_ASK_OVERWRITE;
            g_slot_ask_n = g_state_slot;
         }
         else
         {
            g_slot_saving = g_state_slot;
            act = UI_ACT_SAVESTATE;
         }
      }
      else if ((edges & PSP_CTRL_TRIANGLE) && g_ov_slot[cur])
      {
         g_slot_ask = SLOT_ASK_DELETE;
         g_slot_ask_n = g_state_slot;
      }
   }

   /* THE DWELL: the full picture is read once the cursor has rested
    * OV_SHOT_DWELL frames on a saved slot (this frame counts), with the core
    * paused, and kept until the cursor moves or the slot changes. */
   if (g_ov_dwell <= OV_SHOT_DWELL && ++g_ov_dwell == OV_SHOT_DWELL &&
       g_ov_slot[cur] && g_ov_shot_slot != cur && vid_shot_tex())
   {
      unsigned t0 = sceKernelGetSystemTimeLow();
      int rc = ov_shot_read(cur);
      if (rc != 0)
         g_ov_shot_none[cur] = 1;
      FE_EVT_ONLY(t0);
      fe_evt("shot_dwell slot=%d rc=%d us=%u", cur + 1, rc,
             (unsigned)(sceKernelGetSystemTimeLow() - t0));
   }

   ov_backdrop(OV_SCRIM);
   ov_head(g_ov_title, "SAVE STATES");
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
   {
      int y = SS_ROW_Y(i), by = y + 1, sel = (i == cur);
      char lbl[16];
      /* The band is the drawer card's rule, card = text + 1: two lines,
       * 32 rows, bleeding off the left edge. */
      if (sel)
      {
         vid_rect(0, by, 212, 32, C_ACCENT, 40);
         vid_rect(0, by, 4, 32, C_ACCENT, 255);
      }
      if (g_ov_slot[i] == 2)
         vid_image(20, by + 4, 36, 24,
                   g_ov_thumbs + (size_t)i * PSP_STATE_THUMB_WIDTH *
                      PSP_STATE_THUMB_TEX_HEIGHT,
                   PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_TEX_HEIGHT,
                   PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_HEIGHT, 255);
      else
         vid_rect(20, by + 4, 36, 24, C_CARD, 255);
      snprintf(lbl, sizeof(lbl), "Slot %d", i + 1);
      vid_text(64, y, lbl, sel ? C_SEL : C_ITEM);
      if (g_ov_slot[i])
         vid_text(64, y + 14, g_ov_stamp[i][0] ? g_ov_stamp[i] : "saved",
                  sel ? C_VALUE : C_DIM);
      else
         vid_text(64, y + 14, "empty", C_DIM);
   }
   ss_panel(cur);
   {
      char cap[48];
      snprintf(cap, sizeof(cap), "Slot %d  -  %s", cur + 1,
               !g_ov_slot[cur] ? "empty"
               : g_ov_stamp[cur][0] ? g_ov_stamp[cur] : "saved");
      vid_text(SS_PX, 228, cap, C_DIM);
   }
   if (g_slot_ask)
   {
      char q[72];
      if (g_slot_ask == SLOT_ASK_DELETE)
         snprintf(q, sizeof(q), "Delete Slot %d?      " VID_GLYPH_X
                  " delete     " VID_GLYPH_O " keep", g_slot_ask_n);
      else
         snprintf(q, sizeof(q), "Overwrite Slot %d?      " VID_GLYPH_X
                  " overwrite     " VID_GLYPH_O " keep", g_slot_ask_n);
      ov_plate(q);
   }
   else if (g_slot_note[0])
      ov_plate(g_slot_note);
   else if (g_ov_slot[cur])
      footer(VID_GLYPH_X " load     " VID_GLYPH_SQ " save     " VID_GLYPH_TRI
             " delete     " VID_GLYPH_O " back");
   else
      footer(VID_GLYPH_SQ " save     " VID_GLYPH_O " back");
   return act;
}

/* ----- frame dispatcher --------------------------------------------------- */

ui_action ui_frame(unsigned pad, int session_active, const char *session_info)
{
   unsigned edges;
   ui_action act = UI_ACT_NONE;

   if (!g_active)
      return UI_ACT_NONE;

   if (ui_demo_running())
      pad = demo_pad();
   edges = pad_edges(pad);
   g_ov_linked = session_active;

   vid_overlay_begin(1);
   switch (g_screen)
   {
   case SCR_SETTINGS:
      act = screen_settings(edges);
      break;
   case SCR_WIRELESS:
      act = screen_wireless(edges, session_active, session_info);
      break;
   case SCR_SCAN:
      act = screen_scan(edges);
      break;
   case SCR_MGIFT:
      act = screen_mgift(edges);
      break;
   case SCR_HOST:
      act = screen_host(edges);
      break;
   case SCR_JOINASK:
      act = screen_joinask(edges);
      break;
   case SCR_STATE_SLOTS:
      act = screen_state_slots(edges);
      break;
   case SCR_CONTROLS:
      act = screen_controls(edges, pad);
      break;
#ifdef GPSP_CHEATS
   case SCR_CHEATS:
      if (screen_cheats(edges, g_ov_title))
      {
         screen_to(SCR_MENU);
         g_cursor = M_CHEATS;
      }
      break;
#endif
   default:
      act = screen_menu(edges, session_active);
      break;
   }
   vid_overlay_end();
   demo_dump_flush();
   /* ui_demo: every frame of the menu's opening, for docs/UI-OVERLAY.md. */
   if (g_demo_on >= 0 && g_demo_steps == demo_script &&
       g_screen == SCR_MENU && g_ov_open_k < OV_OPEN_FRAMES &&
       g_ov_open_k > 0)
   {
      char nm[32];
      snprintf(nm, sizeof(nm), "ge_ui_open_%d", g_ov_open_k);
      demo_dump_named(nm);
   }

   if (act == UI_ACT_RESUME || act == UI_ACT_EXIT ||
       act == UI_ACT_NET_HOST || act == UI_ACT_NET_JOIN)
      ui_close();
   return act;
}

/* ----- ROM browser: game gallery ------------------------------------------ */

/* WHY A NAME POOL AND NOT AN ARRAY OF FIXED SLOTS.
 *
 * 2.0 shipped `char name[96]` x 64 and both numbers were wrong the same way:
 * they were sized against MY library.  The first bug report after release was
 * someone with a bit over 128 ROMs seeing exactly half of them, because the
 * scan stopped at 64 entries BEFORE the sort ran -- so the survivors were the
 * first 64 in FAT directory order, roughly the order the files were copied to
 * the stick.  That reads as random, which is the worst way to fail: it looks
 * like the emulator is broken rather than full.
 *
 * Fixed slots are also what forced the 96-character ceiling that silently
 * dropped long No-Intro names.  Pointers into one packed pool spend ~55 bytes
 * on a typical name instead of 96, and put the length limit somewhere no real
 * filename reaches.
 *
 * This is in BSS on purpose.  gpSP's init_gamepak_buffer() mallocs 1 MiB
 * blocks until malloc fails, so once a game is loaded there is no heap left
 * to allocate a list from: a dynamic list would work on the first boot and
 * fail when someone backed out to the game list mid-game, on a PSP-1000
 * only.  BSS is taken before that loop runs, and 112 KB of it costs at most
 * one of those 1 MiB blocks.
 */
#define BROWSER_MAX    1024
#define ROM_POOL_BYTES (96 * 1024)
#define ROM_SCAN_DEPTH 3                /* roms/ plus two levels beneath it */

typedef struct {
   const char *name;    /* path relative to roms/, "Pokemon/Emerald.gba"     */
   const char *base;    /* the filename alone: what the user reads, and what
                         * box art is keyed on, so art never has to mirror
                         * whatever folder layout the ROMs happen to use     */
   unsigned    size;
   signed char has_sav; /* -1 = not looked up yet; see rom_has_sav()         */
   signed char is_fav;  /* in favourites.txt; set by favs_mark(), and it
                         * sits in the padding beside has_sav, so the 1024
                         * entry table costs nothing more for it            */
   unsigned short scan_order; /* preserves traversal order for equal basenames */
} rom_entry;

static rom_entry g_roms[BROWSER_MAX];
static char      g_rom_pool[ROM_POOL_BYTES];
static unsigned  g_rom_pool_used;
static int       g_rom_found;   /* matching ROM files seen, including names
                                 * skipped for path/storage limits; the browser
                                 * keeps this distinct from the usable-entry
                                 * count and says so out loud */
static char      g_rom_root[PSP_FILE_PATH_CAP];
static unsigned  g_rom_scan_path_errors;

/* ---- favourites ----------------------------------------------------------
 *
 * ONE GLOBAL LIST, roms/favourites.txt, one path relative to roms/ per line
 * -- the same key last_rom uses.  The browser only ever lists the ROMs whose
 * extension matches the active console, so the favourites VIEW is
 * per-console without a per-console list: a .gbc line is simply never in a
 * GB scan.  SQUARE toggles the highlighted game and rewrites the file at
 * once (it is a few hundred bytes); SELECT swaps the list for the tagged
 * subset and back.
 *
 * STATIC, like g_roms and for the same reason: the heap is gone once a game
 * has loaded, and this must work when the player backs out to the list. */
static fe_favs        g_favs;                   /* ~14 KB, BSS            */
static int            g_fav_count;              /* tagged entries in scan */
static unsigned short g_view_idx[BROWSER_MAX];  /* favourites view -> rom */
static int            g_view_n;
static int            g_browser_favs;           /* 1 = favourites view    */

/* The list the shells draw is either the scan or the favourites subset;
 * every g_roms[] index that comes from a row goes through here. */
static int view_rom(int i)
{
   return g_browser_favs ? g_view_idx[i] : i;
}

static int view_count(int n_scan)
{
   return g_browser_favs ? g_view_n : n_scan;
}

static int favs_path(const char *rom_dir, char *out, size_t out_sz)
{
   int n = snprintf(out, out_sz, "%s/favourites.txt", rom_dir);
   return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

/* Read straight into the pool and split it in place: no second buffer. */
static void favs_load(const char *rom_dir)
{
   char path[PSP_FILE_PATH_CAP];
   SceUID fd;
   int len = 0;
   fe_favs_clear(&g_favs);
   if (favs_path(rom_dir, path, sizeof(path)) == 0 &&
       (fd = sceIoOpen(path, PSP_O_RDONLY, 0)) >= 0)
   {
      len = sceIoRead(fd, g_favs.pool, FE_FAVS_POOL);
      sceIoClose(fd);
      if (len > 0)
         fe_favs_parse_pool(&g_favs, (size_t)len);
   }
   fe_evt("favs_load n=%u bytes=%d", g_favs.n, len);
}

static int favs_save(const char *rom_dir)
{
   char path[PSP_FILE_PATH_CAP];
   SceUID fd;
   unsigned i;
   int rc = 0;
   if (favs_path(rom_dir, path, sizeof(path)) != 0)
      return -1;
   fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd < 0)
      rc = -1;
   for (i = 0; rc == 0 && i < g_favs.n; i++)
   {
      const char *s = g_favs.pool + g_favs.off[i];
      int len = (int)strlen(s);
      if (sceIoWrite(fd, s, len) != len || sceIoWrite(fd, "\n", 1) != 1)
         rc = -1;
   }
   if (fd >= 0)
      sceIoClose(fd);
   fe_evt("favs_save n=%u rc=%d", g_favs.n, rc);
   return rc;
}

/* Stamp is_fav on every scan entry and rebuild the favourites view.  Once
 * per scan and per toggle, never per frame: 1024 hashes of a filename is
 * nothing, 1024 strcmp per row per frame would not be. */
static void favs_build_view(int n_scan)
{
   int i;
   g_view_n = 0;
   for (i = 0; i < n_scan; i++)
      if (g_roms[i].is_fav)
         g_view_idx[g_view_n++] = (unsigned short)i;
}

static void favs_mark(int n_scan)
{
   int i;
   g_fav_count = 0;
   for (i = 0; i < n_scan; i++)
   {
      g_roms[i].is_fav = fe_favs_find(&g_favs, g_roms[i].name) >= 0;
      g_fav_count += g_roms[i].is_fav;
   }
   favs_build_view(n_scan);
}

/* ---- the console switch ---------------------------------------------------
 *
 * TRIANGLE is a moment, not a filter change.  The rows fan out to the right
 * (lower rows lagging) while the title, meta line and hero fade; the NEW
 * console's palette slides in from the left as K vertical bands, each two
 * frames behind the last, until the screen IS the palette -- the DMG's four
 * greens, the GBC's five shells, the GBA's three indigos -- with the
 * console's name on it.  The bands then leave to the right in the same
 * order and the new list slides in beneath them.  550 ms for GBA, 617 for
 * GB, 683 for GBC.
 *
 * THE RESCAN HAPPENS AT THE PEAK, when the bands cover everything.  Walking
 * roms/ is a directory read per folder and stalls the frame; behind an
 * opaque screen a stall is invisible.  A second TRIANGLE mid-flare finishes
 * it at once (rescanning if that has not happened yet) and starts the next.
 *
 * Every step is rects and text: K vid_rect for the bands, one for the
 * caption plate, text colours walked toward the background for the fades.
 * No allocation, no texture. */
#define SW_LAG     2          /* frames between one band and the next     */
#define SW_TRAVEL  9          /* frames a band takes to cross the screen  */
#define SW_OUT     12         /* frames the old rows take to fan out      */
#define SW_ROWS_IN 10         /* frames the new rows take to slide in     */

static struct {
   int active;
   int f;                    /* frames since the press                    */
   int to;                   /* the console being switched to             */
   int swapped;              /* the rescan has happened                   */
   int peak, exit_start, rows_in, end;
} g_sw;

/* Presentation modifiers the shells read.  All 0..256 fixed point. */
static int g_fx_out;         /* fan-out progress of the outgoing rows     */
static int g_fx_in;          /* frames since the new rows began arriving,
                              * -1 when they are not arriving             */
static int g_fx_badge;       /* badge scale, 256 = 1:1                    */
static int g_fx_fade;        /* list/title colour walked toward the bg    */
static int g_fx_dy;          /* list/title vertical shift, pixels         */

static void fx_reset(void)
{
   g_fx_out = 0;
   g_fx_in = -1;
   g_fx_badge = 256;
   g_fx_fade = 0;
   g_fx_dy = 0;
}

static int clamp256(int v)
{
   return v < 0 ? 0 : v > 256 ? 256 : v;
}

static int ease_out(int u)            /* 1 - (1-u)^3 */
{
   int v = 256 - clamp256(u);
   return 256 - (((v * v) >> 8) * v >> 8);
}

static int ease_in(int u)             /* u^2 */
{
   u = clamp256(u);
   return (u * u) >> 8;
}

/* Per-row offset and fade for list row `rel` (-3..3 around the cursor). */
static void row_fx(int rel, int *dx, int *fade)
{
   *dx = 0;
   *fade = g_fx_fade;
   if (g_fx_out > 0)
   {
      int e = ease_in(g_fx_out);
      *dx = (e * (56 + 12 * (rel + 3))) >> 8;
      *fade = g_fx_out;
   }
   else if (g_fx_in >= 0)
   {
      int p = ease_out(((g_fx_in - (rel + 3)) * 256) / SW_ROWS_IN);
      *dx = -((40 * (256 - p)) >> 8);
      *fade = 256 - p;
   }
}

static void switch_begin(int to)
{
   int k = skin_of(to)->pal_n;
   g_sw.active = 1;
   g_sw.f = 0;
   g_sw.to = to;
   g_sw.swapped = 0;
   g_sw.peak = 4 + (k - 1) * SW_LAG + SW_TRAVEL;
   g_sw.exit_start = g_sw.peak + 3;
   g_sw.rows_in = g_sw.exit_start + 2;
   g_sw.end = g_sw.exit_start + (k - 1) * SW_LAG + SW_TRAVEL;
   fx_reset();
}

/* Advance one frame and set the modifiers.  Returns 1 on the frame the
 * caller must perform the rescan. */
static int switch_step(void)
{
   int do_swap = 0;
   if (!g_sw.active)
      return 0;
   g_sw.f++;
   if (!g_sw.swapped)
   {
      g_fx_out = clamp256((g_sw.f * 256) / SW_OUT);
      g_fx_in = -1;
      if (g_sw.f >= g_sw.peak)
         do_swap = 1;
   }
   else
   {
      int bp = clamp256(((g_sw.f - g_sw.exit_start) * 256) / 10);
      g_fx_out = 0;
      g_fx_in = g_sw.f - g_sw.rows_in;
      /* Lands at 1.35x and settles: the badge is what changed. */
      g_fx_badge = 256 + ((90 * (((256 - bp) * (256 - bp)) >> 8)) >> 8);
      if (g_sw.f >= g_sw.end)
      {
         g_sw.active = 0;
         fx_reset();
      }
   }
   return do_swap;
}

/* The bands and the caption, over everything the shell drew. */
static void switch_draw(void)
{
   const ui_skin *s = skin_of(g_sw.to);
   int k, f = g_sw.f;
   if (!g_sw.active)
      return;
   for (k = 0; k < s->pal_n; k++)
   {
      int x0 = (k * 480) / s->pal_n, x1 = ((k + 1) * 480) / s->pal_n, x;
      int ep = clamp256(((f - 4 - k * SW_LAG) * 256) / SW_TRAVEL);
      int xp = clamp256(((f - g_sw.exit_start - k * SW_LAG) * 256) / SW_TRAVEL);
      if (ep <= 0)
         continue;
      if (xp > 0)
         x = x0 + ((480 * ease_in(xp)) >> 8);
      else
         x = x0 - ((480 * (256 - ease_out(ep))) >> 8);
      vid_rect(x, 0, x1 - x0 + 1, 272, s->pal[k], 255);
   }
   /* Caption: it names what you switched to, on a plate so it reads on the
    * yellow band as well as the dark green one.  Rises 6 px as it lands and
    * cuts the moment the first band starts to leave -- the motion hides the
    * cut, and text has no alpha to fade with. */
   if (f >= g_sw.peak - 5 && f < g_sw.exit_start + 2)
   {
      int tp = clamp256(((f - (g_sw.peak - 5)) * 256) / 6);
      int w = vid_text_hd_w(s->id), fw = vid_text_w(s->full), pw, py;
      if (fw > w)
         w = fw;
      pw = w + 36;
      py = 112 - ((6 * (256 - tp)) >> 8);
      vid_rect((480 - pw) / 2, py, pw, 52, 0x0000, (150 * tp) >> 8);
      vid_rect((480 - pw) / 2, py, 3, 52, 0xFFFF, (255 * tp) >> 8);
      if (tp >= 128)
      {
         vid_text_hd((480 - vid_text_hd_w(s->id)) / 2, py + 6, s->id, 0xFFFF);
         vid_text((480 - fw) / 2, py + 30, s->full, 0xCE59);
      }
   }
}

/* ---- the favourite star: tag, pop, sparks --------------------------------
 * Frames 0..13 after SQUARE.  Tagging: the star arrives large (the hd face's
 * star, 1.4x the row's) and settles to the row size at frame 10, while six
 * 2x2 sparks radiate and the selection edge flashes white.  Untagging: the
 * star dims, turns to its outline at frame 5, and is gone at 14.  The row
 * itself never moves. */
#define POP_FRAMES 14

static struct {
   int active;
   int f;
   int add;
   int view;                 /* the row's index in the current view      */
} g_pop;

static void pop_begin(int view, int add)
{
   g_pop.active = 1;
   g_pop.f = 0;
   g_pop.add = add;
   g_pop.view = view;
}

/* Returns 1 on the frame the pop finishes. */
static int pop_step(void)
{
   if (!g_pop.active)
      return 0;
   if (++g_pop.f >= POP_FRAMES)
   {
      g_pop.active = 0;
      return 1;
   }
   return 0;
}

/* `x` is the star column, `y_cur` the top of the cursor row this frame,
 * `row_h` the pitch: the pop stays with its row if the list moves on. */
static void browser_pop_draw(int x, int y_cur, int row_h, int cur)
{
   int f = g_pop.f, y, k;
   if (!g_pop.active)
      return;
   y = y_cur + (g_pop.view - cur) * row_h;
   if (g_pop.add)
   {
      /* Two sizes is what a bitmap font has: the hd star (1.4x) for the
       * overshoot, the row star once it settles.  The hd glyph is offset
       * so both share a centre. */
      if (f < 2)
         vid_text(x, y, VID_GLYPH_STAR_O, C_CON);
      else if (f < 10)
         vid_text_hd(x - 1, y - 3, VID_GLYPH_STAR, C_CON);
      else
         vid_text(x, y, VID_GLYPH_STAR, C_CON);
      for (k = 0; k < 6; k++)
      {
         /* Six sparks on a 60-degree fan, radius 4 -> 15, fading out.  A
          * sin/cos table for six fixed angles, in 256ths. */
         static const int sx[6] = { 245, 45, -200, -245, -45, 200 };
         static const int sy[6] = { 75, 252, 177, -75, -252, -177 };
         int r = 4 + ((11 * ease_out((f * 256) / POP_FRAMES)) >> 8);
         int a = 255 - (255 * f) / POP_FRAMES;
         vid_rect(x + 6 + ((sx[k] * r) >> 8) - 1, y + 9 + ((sy[k] * r) >> 8) - 1,
                  2, 2, C_CON, a);
      }
   }
   else
   {
      if (f < 5)
         vid_text(x, y, VID_GLYPH_STAR, C_DIM);
      else
         vid_text(x, y, VID_GLYPH_STAR_O,
                  mix565(C_DIM, C_BG_TOP, ((f - 5) * 256) / (POP_FRAMES - 5)));
   }
}

/* ---- header: the two VIEW switches ---------------------------------------
 *
 * The footer was full.  Its five hints already ended at x=469, and the two
 * new controls did not fit at any abbreviation, so the browser now splits
 * them by kind: the footer keeps the ACTIONS on the highlighted game (play,
 * page, states, star, settings) and the header carries the two VIEW switches
 * -- TRIANGLE beside the console card, SELECT beside the favourites pill --
 * each labelled with its button, right where the thing it changes is drawn.
 *
 * Right to left: the console card (hdr_card, the rightmost thing, exactly
 * where the loading screen puts it), TRIANGLE hung on it, then the
 * favourites pill with SELECT before it, then the games count when it fits.
 * (3.1 had the pill outermost and the card inside it; the owner moved the
 * card out, so a picked game's loading screen keeps it in place.)
 * Favourites pill: the star and how many, filled in the console colour
 * while that view is open. */
static void browser_header(int n_scan)
{
   const ui_skin *s = skin_of(g_pcfg.console);
   char count[8];
   int x, pw, i;

   x = hdr_card(s, g_fx_badge);
   x -= 4 + vid_text_w(VID_GLYPH_TRI);
   vid_text(x, HDR_TEXT_Y, VID_GLYPH_TRI, C_DIM);

   snprintf(count, sizeof(count), "%d", g_fav_count);
   pw = 8 + vid_text_w(VID_GLYPH_STAR) + 4 + vid_text_w(count) + 8;
   x -= 14 + pw;
   if (g_browser_favs)
   {
      vid_rect(x, HDR_CARD_Y, pw, HDR_CARD_H, C_CON, 60);
      vid_text(x + 8, HDR_TEXT_Y, VID_GLYPH_STAR, C_CON);
      vid_text(x + 8 + vid_text_w(VID_GLYPH_STAR) + 4, HDR_TEXT_Y, count,
               C_SEL);
   }
   else
   {
      /* The count is information, not a hint: C_ACCENT_DK (#6B6B6B on the
       * light page, #8B8B8B on the dark) clears 4.5:1 where C_DIM does
       * not.  The SELECT and TRIANGLE labels stay C_DIM like the footer. */
      vid_rect(x, HDR_CARD_Y, pw, HDR_CARD_H, C_BG_TOP, 140);
      vid_text(x + 8, HDR_TEXT_Y,
               g_fav_count ? VID_GLYPH_STAR : VID_GLYPH_STAR_O, C_ACCENT_DK);
      vid_text(x + 8 + vid_text_w(VID_GLYPH_STAR) + 4, HDR_TEXT_Y, count,
               C_ACCENT_DK);
   }
   x -= 6 + vid_text_w("SELECT");
   vid_text(x, HDR_TEXT_Y, "SELECT", C_DIM);

   /* The shelf used to put "24 games" here; it keeps it when it fits. */
   if (!g_ui_shell && n_scan > 0)
   {
      char buf[24];
      snprintf(buf, sizeof(buf), "%d games", n_scan);
      i = x - 16 - vid_text_w(buf);
      if (i >= HDR_LOGO_X + VID_LOGO_W + 12)
         vid_text(i, HDR_TEXT_Y, buf, C_DIM);
   }
}

/* One footer for both shells.  Text is proportional, so the hints are laid
 * out by measured width.  Honest per state: the square hint says which way
 * the highlighted game will go, and an empty list advertises only what
 * still works. */
static void browser_footer(int n_view, int cur_is_fav)
{
   const char *hint[6];
   int count = 0, x = 20, i;
   if (g_browser_state_open)
      return;
   if (n_view <= 0)
   {
      hint[count++] = g_browser_favs ? "SELECT all games"
                                     : VID_GLYPH_O " exit";
      hint[count++] = "START settings";
   }
   else
   {
      hint[count++] = VID_GLYPH_X " play";
#ifdef GPSP_CHEATS
      /* "L/R page" gave its place to the cheats hint: the row is full (the
       * shoulder buttons still page the list). */
      hint[count++] = "Left: states";
      hint[count++] = "Right: cheats";
#else
      hint[count++] = "L/R page";
      hint[count++] = "Left: states";
#endif
      hint[count++] = cur_is_fav ? VID_GLYPH_SQ " unstar" : VID_GLYPH_SQ " star";
      hint[count++] = "START settings";
   }
   for (i = 0; i < count; i++)
   {
      vid_text(x, 252, hint[i], C_DIM);
      x += vid_text_w(hint[i]) + 18;
   }
}

/* One stat per ROM during the scan was ~2 s of boot on a large library, and
 * it was wasted work: only the handful of entries actually drawn need this. */
static int rom_has_sav(int idx)
{
   if (g_roms[idx].has_sav < 0)
   {
      char sav[PSP_FILE_PATH_CAP];
      SceIoStat st;
      /* GB/GBC saves keep the .gb/.gbc stem so a GB and a GBA game of the
       * same name cannot share one battery file. */
      int n = g_pcfg.console == FE_CONSOLE_GBA
         ? snprintf(sav, sizeof(sav), "%s/%.*s.sav", g_rom_root,
                    (int)ui_rom_stem_length(g_roms[idx].name),
                    g_roms[idx].name)
         : snprintf(sav, sizeof(sav), "%s/%s.sav", g_rom_root,
                    g_roms[idx].name);
      if (n < 0 || (size_t)n >= sizeof(sav))
      {
         g_roms[idx].has_sav = 0;
         return 0;
      }
      g_roms[idx].has_sav = (signed char)(sceIoGetstat(sav, &st) >= 0);
   }
   return g_roms[idx].has_sav;
}

static const char *rom_pool_add(const char *s, size_t len)
{
   char *p;
   if (g_rom_pool_used + len + 1 > sizeof(g_rom_pool))
      return NULL;
   p = g_rom_pool + g_rom_pool_used;
   memcpy(p, s, len);
   p[len] = '\0';
   g_rom_pool_used += (unsigned)(len + 1);
   return p;
}

/* `rel` is "" at the top level, otherwise a path under roms/ with no leading
 * or trailing slash.  Directories are walked because sorting a library into
 * folders is the obvious thing to do with one, and 2.0 ignored every one. */
static void rom_scan_dir(const char *rel, int depth, int *n,
                         fe_console_t console)
{
   char dir[PSP_FILE_PATH_CAP];
   SceUID d;
   SceIoDirent ent;

   if (rel[0])
   {
      if (psp_rom_path_join(dir, sizeof(dir), g_rom_root, rel) != 0)
      {
         g_rom_scan_path_errors++;
         fe_evt("rom_scan_skip reason=full_path_too_long rel=%s", rel);
         return;
      }
   }
   else
      snprintf(dir, sizeof(dir), "%s", g_rom_root);

   d = sceIoDopen(dir);
   if (d < 0)
      return;
   memset(&ent, 0, sizeof(ent));
   while (sceIoDread(d, &ent) > 0)
   {
      /* Memory Stick reports a directory in st_mode on some firmwares and
       * only in st_attr on others.  Test both, rather than pick one and find
       * out which from a bug report. */
      int is_dir = (ent.d_stat.st_mode & 0x1000) != 0 ||
                   (ent.d_stat.st_attr & 0x0010) != 0;

      if (is_dir)
      {
         /* Skips "." and ".." and anything else dot-prefixed, which on a
          * stick that has been near a Mac means __MACOSX and .Trashes. */
         if (depth > 1 && ent.d_name[0] != '.')
         {
            char sub[PSP_ROM_REL_PATH_CAP];
            int sub_len;
            if (rel[0])
               sub_len = snprintf(sub, sizeof(sub), "%s/%s", rel, ent.d_name);
            else
               sub_len = snprintf(sub, sizeof(sub), "%s", ent.d_name);
            if (sub_len < 0 || (size_t)sub_len >= sizeof(sub))
            {
               g_rom_scan_path_errors++;
               fe_evt("rom_scan_skip reason=relative_path_too_long dir=%s",
                      ent.d_name);
            }
            else
               rom_scan_dir(sub, depth - 1, n, console);
         }
      }
      else if (ui_rom_matches_console(ent.d_name, console))
      {
         g_rom_found++;
         ret_scan_tick();
         if (*n < BROWSER_MAX)
         {
            char        path[PSP_ROM_REL_PATH_CAP];
            const char *stored;
            int         plen;

            if (rel[0])
               plen = snprintf(path, sizeof(path), "%s/%s", rel, ent.d_name);
            else
               plen = snprintf(path, sizeof(path), "%s", ent.d_name);

            if (plen > 0 && plen < (int)sizeof(path) &&
                (stored = rom_pool_add(path, (size_t)plen)) != NULL)
            {
               const char *slash = strrchr(stored, '/');
               g_roms[*n].name    = stored;
               g_roms[*n].base    = slash ? slash + 1 : stored;
               g_roms[*n].size    = (unsigned)ent.d_stat.st_size;
               g_roms[*n].has_sav = -1;
               g_roms[*n].is_fav  = 0;       /* favs_mark() after the sort */
               g_roms[*n].scan_order = (unsigned short)*n;
               (*n)++;
            }
            else if (plen < 0 || plen >= (int)sizeof(path))
            {
               g_rom_scan_path_errors++;
               fe_evt("rom_scan_skip reason=relative_path_too_long file=%s",
                      ent.d_name);
            }
         }
      }
      memset(&ent, 0, sizeof(ent));
   }
   sceIoDclose(d);
}

static int rom_entry_less(const rom_entry *a, const rom_entry *b)
{
   int by_name = strcasecmp(a->base, b->base);
   return by_name < 0 || (by_name == 0 && a->scan_order < b->scan_order);
}

static void rom_heap_sift_down(int root, int end)
{
   while (root * 2 + 1 < end)
   {
      int child = root * 2 + 1;
      rom_entry tmp;
      if (child + 1 < end && rom_entry_less(&g_roms[child], &g_roms[child + 1]))
         child++;
      if (!rom_entry_less(&g_roms[root], &g_roms[child]))
         return;
      tmp = g_roms[root];
      g_roms[root] = g_roms[child];
      g_roms[child] = tmp;
      root = child;
   }
}

/* In-place heap sort by basename. The old insertion sort could compare and
 * shift roughly half a million entries for a full, reverse-ish 1024-ROM
 * library. Heap sort uses O(n log n) comparisons and no extra BSS/heap. The
 * scan ordinal preserves traversal order for equal basenames. */
static void rom_sort(int n)
{
   int start, end;
   for (start = n / 2 - 1; start >= 0; start--)
      rom_heap_sift_down(start, n);
   for (end = n - 1; end > 0; end--)
   {
      rom_entry tmp = g_roms[0];
      g_roms[0] = g_roms[end];
      g_roms[end] = tmp;
      rom_heap_sift_down(0, end);
   }
}

static int rom_scan(const char *rom_dir, fe_console_t console)
{
   int n = 0;

   g_rom_pool_used = 0;
   g_rom_found     = 0;
   g_rom_scan_path_errors = 0;
   {
      int root_len = snprintf(g_rom_root, sizeof(g_rom_root), "%s", rom_dir);
      if (root_len < 0 || (size_t)root_len >= sizeof(g_rom_root))
      {
         g_rom_root[0] = '\0';
         return -1;
      }
   }

   rom_scan_dir("", ROM_SCAN_DEPTH, &n, console);

   /* Sort after collecting the full tree: truncating before sorting was the
    * original 2.0 browser bug. */
   rom_sort(n);
   return n;
}

/* ---- box art -------------------------------------------------------------
 *
 * Box art is PORTRAIT (a GBA box is roughly 5:7), and it used to be squashed
 * into a 112-square and then, in the marquee shell, stretched 4.3x to fill
 * the screen.  Both are gone: art keeps its aspect, and there are two tiers
 * so neither view is fed a texture built for the other.
 *
 *   THUMB  128x176 inside a 128x256 texture (GE demands power-of-two dims),
 *          64 KiB a slot.  Ten slots is a screenful plus what you are about
 *          to scroll onto.
 *   HERO   256x358 inside a 256x512 texture, 256 KiB, exactly one of them,
 *          rebuilt when the selection settles.  The marquee draws it at
 *          480 wide -- a 1.9x stretch instead of 4.3x.
 *
 * Both are freed when the browser exits, so this pool costs the running
 * emulator nothing.
 *
 * SOURCE FILES: `<appdir>/boxart/<rom-name-minus-.gba>.<ext>`, tried as .png
 * then .jpg then .bmp.  PNG/JPEG go through stb_image (whole-file decode);
 * BMP keeps the old row-streaming reader, which needs no buffer at all.
 * Feed it the highest-resolution scan you have: the resampler below is a BOX
 * FILTER, so a 1000px source genuinely looks better than a 200px one.  Point
 * sampling a big scan down to 176px aliases so badly it looks WORSE than a
 * small source, which is the trap the previous version fell into.
 */
#define ART_TEX_W   128
#define ART_TEX_H   256
#define ART_W       128
#define ART_H       176
/* HERO TEXTURE SIZE IS CHOSEN AT RUNTIME.
 *
 * The GE samples power-of-two textures only, and the PSP screen is 480x272 --
 * not a power of two in either axis.  A 512x512 texture is the smallest that
 * holds a full-screen image, so pre-composed art lands in it at NATIVE size
 * and is drawn 1:1: no downsample, no upscale, exactly the pixels the artist
 * made.  It costs 512 KiB.
 *
 * A 256x256 texture costs 128 KiB but forces 480 -> 256 -> 480, which is two
 * lossy resamples and visibly soft.  It exists only as the fallback.
 *
 * WHICH ONE IS NOT DECIDED BY CONSOLE MODEL.  A PSP-1000 has half the RAM of
 * a 2000/3000/Go, so model would be a fair proxy -- but only a proxy.  What
 * actually matters is whether half a megabyte is available CONTIGUOUSLY at
 * this moment, and a field log has shown max_block as low as 196 KiB on a
 * 64 MB console.  So ask the allocator, which answers the real question and
 * also covers the fragmented case model detection would miss. */
#define HERO_TEX_BIG    512
#define HERO_TEX_SMALL  256
#define HERO_W      256
#define HERO_H      358
#define ART_SLOTS   10

/* THE 32 MB CONSOLE IS THE ONE THAT MATTERS.  A PSP-1000 has half the RAM of
 * a 2000/3000, and this pool is the largest thing the frontend allocates
 * outside the core.  Ten thumbs is 640 KiB, the hero another 256 KiB, and a
 * PNG decode transiently wants the file plus w*h*3 on top -- comfortably over
 * a megabyte at the worst moment.  A field log showed max_block=200704 while
 * the hero alone asks for 256 KiB contiguous, so "there is plenty of RAM" was
 * never true; it was only ever true on the 64 MB unit this was tested on.
 *
 * So every allocation here is CONDITIONAL on the console being able to spare
 * it, and every failure degrades to no-art rather than to a wedged browser. */
/* NO PRE-FLIGHT BUDGET CHECK.  There was one, and it broke all box art.
 *
 * It asked sceKernelTotalFreeMemSize()/MaxFreeMemSize() whether an allocation
 * would fit.  Those report the KERNEL PARTITION, but main_psp.c declares
 * PSP_HEAP_SIZE_KB(-1024) -- "give the heap everything except 1 MB" -- so
 * malloc draws from a pool those functions do not describe.  They returned a
 * constant ~750 KB no matter how much heap was free, and a 2 MB floor on top
 * of that denied every request, down to a 64 KB thumbnail.  Observed live:
 * "art_budget deny want=65536 block=524288 free=765952", repeated for every
 * cover in the library.
 *
 * The allocation IS the test.  memalign returns NULL when it cannot be
 * satisfied, every caller here already handles NULL by degrading to no art,
 * and that path costs nothing when memory is plentiful. */

/* Decode ceiling.  A 2000x2800 scan is 16 MB decoded, which the browser can
 * afford but nothing is gained by it; stb is asked to fail beyond this so a
 * pathological file cannot take the frontend down with it. */
#define ART_SRC_MAX_PX (1600 * 1600)

/* aw/ah is the sub-rect actually filled.  Box-art scans are not all the
 * same shape -- libretro boxarts are square, a real scan is 5:7 -- so the
 * image is fitted INSIDE the panel at its own aspect and the caller is
 * told what it got.  Stretching every cover to one frame is what makes a
 * gallery look wrong without the eye being able to say why. */
typedef struct { int rom_idx; int state; int aw, ah; uint16_t *tex; } art_slot;
static art_slot g_art[ART_SLOTS];
static int g_art_clock;
static uint16_t *g_hero;
static int g_hero_idx = -1;
static int g_hero_state;
static int g_hero_tex;          /* edge of the allocated texture, 512 or 256 */
static int g_hero_w, g_hero_h;
/* 1 when the backdrop came from hero/ -- artwork a person composed FOR this
 * shell, already dark and already quiet on the left.  The heavy scrim exists
 * to beat raw box art into something text can sit on; applying it to art that
 * has already been treated crushes it to black. */
static int g_hero_precomposed;

static void art_free_all(void)
{
   int i;
   for (i = 0; i < ART_SLOTS; i++)
   {
      free(g_art[i].tex);
      g_art[i].tex = NULL;
      g_art[i].state = 0;
      g_art[i].rom_idx = -1;
   }
   free(g_hero);
   g_hero = NULL;
   g_hero_idx = -1;
   g_hero_state = 0;
   g_hero_tex = 0;
}

/* Box-average `src` (w x h, `nc` bytes per pixel, R first) into an RGB565
 * dw x dh image at the top-left of a `stride`-wide texture.  Averaging is
 * what makes a high-resolution scan pay off; nearest sampling throws away
 * 95% of the pixels and keeps whichever ones happen to land on the grid. */
static void art_resample(const unsigned char *src, int w, int h, int nc,
                         uint16_t *dst, int stride, int dw, int dh)
{
   int dy;
   for (dy = 0; dy < dh; dy++)
   {
      int y0 = dy * h / dh, y1 = (dy + 1) * h / dh;
      int dx;
      if (y1 <= y0) y1 = y0 + 1;
      for (dx = 0; dx < dw; dx++)
      {
         int x0 = dx * w / dw, x1 = (dx + 1) * w / dw;
         unsigned r = 0, g = 0, b = 0, n = 0;
         int sx, sy;
         if (x1 <= x0) x1 = x0 + 1;
         for (sy = y0; sy < y1; sy++)
         {
            const unsigned char *row = src + (size_t)sy * w * nc;
            for (sx = x0; sx < x1; sx++)
            {
               const unsigned char *p = row + (size_t)sx * nc;
               r += p[0]; g += p[1]; b += p[2];
               n++;
            }
         }
         if (!n) n = 1;
         r /= n; g /= n; b /= n;
         dst[dy * stride + dx] = (uint16_t)((r >> 3) | ((g >> 2) << 5) |
                                            ((b >> 3) << 11));
      }
   }
}

/* Read a whole file into a malloc'd buffer. */
static unsigned char *art_slurp(const char *path, int *out_len)
{
   SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
   int len, got;
   unsigned char *buf;

   if (f < 0)
      return NULL;
   len = sceIoLseek32(f, 0, PSP_SEEK_END);
   sceIoLseek32(f, 0, PSP_SEEK_SET);
   if (len <= 0 || len > 8 * 1024 * 1024)
   {
      sceIoClose(f);
      return NULL;
   }
   buf = (unsigned char *)malloc((size_t)len);
   if (!buf)
   {
      sceIoClose(f);
      return NULL;
   }
   got = sceIoRead(f, buf, len);
   sceIoClose(f);
   if (got != len)
   {
      free(buf);
      return NULL;
   }
   *out_len = len;
   return buf;
}

/* Largest w x h inside the box that keeps the source aspect. */
static void art_fit(int sw, int sh, int boxw, int boxh, int *ow, int *oh)
{
   int w = boxw, h = (int)((long)boxw * sh / (sw ? sw : 1));
   if (h > boxh)
   {
      h = boxh;
      w = (int)((long)boxh * sw / (sh ? sh : 1));
   }
   *ow = w < 1 ? 1 : w;
   *oh = h < 1 ? 1 : h;
}

/* PNG/JPEG through stb_image.  Returns 0 on success and reports the fitted
 * size back through dw and dh, at most the box passed in. */
static int art_load_stb(const char *path, uint16_t *tex, int stride,
                        int *dw, int *dh)
{
   int flen = 0, w = 0, h = 0, nc = 0, fw, fh;
   unsigned char *file = art_slurp(path, &flen);
   unsigned char *pix;

   if (!file)
      return -1;
   if (!stbi_info_from_memory(file, flen, &w, &h, &nc) ||
       w <= 0 || h <= 0 || w > ART_SRC_MAX_PX / h)
   {
      fe_evt("art_fail stage=info path=%s w=%d h=%d", path, w, h);
      free(file);
      return -1;
   }
   pix = stbi_load_from_memory(file, flen, &w, &h, &nc, 3);
   free(file);
   if (!pix)
   {
      fe_evt("art_fail stage=decode path=%s", path);
      return -1;
   }
   art_fit(w, h, *dw, *dh, &fw, &fh);
   art_resample(pix, w, h, 3, tex, stride, fw, fh);
   stbi_image_free(pix);
   fe_evt("art_load path=%s src=%dx%d -> %dx%d", path, w, h, fw, fh);
   *dw = fw;
   *dh = fh;
   return 0;
}

/* Uncompressed BMP, streamed a row at a time so a big cover never needs a
 * whole-file buffer.  Kept because it costs nothing and a card full of BMPs
 * from an older install must keep working.  Point-sampled: a BMP big enough
 * to alias is a BMP that should have been a PNG. */
static int art_load_bmp(const char *path, uint16_t *tex, int stride,
                        int *pdw, int *pdh)
{
   int dw = *pdw, dh = *pdh;
   unsigned char hdr[54];
   unsigned char rowbuf[8192];
   SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
   int w, h, bpp, comp, topdown, dy;
   unsigned off, rowbytes;

   if (f < 0)
      return -1;
   if (sceIoRead(f, hdr, 54) != 54 || hdr[0] != 'B' || hdr[1] != 'M')
   {
      fe_evt("art_fail stage=hdr path=%s", path);
      sceIoClose(f);
      return -1;
   }
   off  = hdr[10] | (hdr[11] << 8) | ((unsigned)hdr[12] << 16) |
          ((unsigned)hdr[13] << 24);
   w    = (int)(hdr[18] | (hdr[19] << 8) | ((unsigned)hdr[20] << 16) |
          ((unsigned)hdr[21] << 24));
   h    = (int)(hdr[22] | (hdr[23] << 8) | ((unsigned)hdr[24] << 16) |
          ((unsigned)hdr[25] << 24));
   bpp  = hdr[28] | (hdr[29] << 8);
   comp = hdr[30] | (hdr[31] << 8) | ((unsigned)hdr[32] << 16) |
          ((unsigned)hdr[33] << 24);
   topdown = 0;
   if (h < 0) { h = -h; topdown = 1; }
   if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || comp != 0 ||
       (bpp != 24 && bpp != 32))
   {
      fe_evt("art_fail stage=fmt path=%s w=%d h=%d bpp=%d comp=%d",
             path, w, h, bpp, comp);
      sceIoClose(f);
      return -1;
   }
   rowbytes = ((unsigned)w * (bpp / 8) + 3) & ~3u;
   if (rowbytes > sizeof(rowbuf))
   {
      fe_evt("art_fail stage=rowbuf path=%s rowbytes=%u", path, rowbytes);
      sceIoClose(f);
      return -1;
   }
   art_fit(w, h, dw, dh, &dw, &dh);
   *pdw = dw;
   *pdh = dh;

   for (dy = 0; dy < dh; dy++)
   {
      int sy = dy * h / dh;
      int fy = topdown ? sy : (h - 1 - sy);
      int dx;
      uint16_t *dst = &tex[dy * stride];
      if (sceIoLseek32(f, (int)(off + (unsigned)fy * rowbytes),
                       PSP_SEEK_SET) < 0 ||
          sceIoRead(f, rowbuf, rowbytes) != (int)rowbytes)
      {
         fe_evt("art_fail stage=read path=%s dy=%d fy=%d off=%u", path, dy,
                fy, off);
         sceIoClose(f);
         return -1;
      }
      for (dx = 0; dx < dw; dx++)
      {
         const unsigned char *p = &rowbuf[(dx * w / dw) * (bpp / 8)];
         /* BMP is B,G,R[,A]; GE 5650 texel is PSP channel order (R low). */
         dst[dx] = (uint16_t)((p[2] >> 3) | ((p[1] >> 2) << 5) |
                              ((p[0] >> 3) << 11));
      }
   }
   sceIoClose(f);
   return 0;
}

/* ---- .565: the texture, already made ---------------------------------
 *
 * The GE samples RGB565.  A PNG costs an inflate, a per-byte unfilter pass,
 * an 888->565 conversion over 130k pixels and then a copy -- to produce bytes
 * that could simply have been stored.  A .565 file IS the texture, laid out
 * at the texture stride, so loading it is ONE sceIoRead into the destination.
 *
 * No fidelity is lost: the texture is 16-bit whichever route the pixels take.
 * The conversion merely happens on a PC instead, where it can be dithered --
 * which the truncation below cannot -- so gradients come out cleaner.
 *
 * Header is 16 bytes, so the pixel data stays 16-byte aligned for the read.
 * See tools/heroforge/raw565.py for the writer. */
#define R565_MAGIC  0x52353635u        /* '565R' little-endian */
#define R565_HDR    16

static int art_load_565(const char *path, uint16_t *tex, int stride,
                        int *dw, int *dh)
{
   unsigned char hdr[R565_HDR];
   SceUID f = sceIoOpen(path, PSP_O_RDONLY, 0);
   unsigned magic, fstride;
   int w, h, y, got;

   if (f < 0)
      return -1;
   if (sceIoRead(f, hdr, R565_HDR) != R565_HDR)
   {
      sceIoClose(f);
      return -1;
   }
   magic   = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
             ((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
   w       = hdr[4] | (hdr[5] << 8);
   h       = hdr[6] | (hdr[7] << 8);
   fstride = hdr[8] | (hdr[9] << 8);
   if (magic != R565_MAGIC || w <= 0 || h <= 0 ||
       w > stride || h > stride || fstride < (unsigned)w ||
       fstride > (unsigned)stride)
   {
      fe_evt("art_fail stage=565hdr path=%s w=%d h=%d stride=%u",
             path, w, h, fstride);
      sceIoClose(f);
      return -1;
   }

   if ((int)fstride == stride)
   {
      /* The file matches the texture: one read, no repacking at all. */
      got = sceIoRead(f, tex, stride * h * 2);
      if (got != stride * h * 2)
      {
         fe_evt("art_fail stage=565read path=%s got=%d", path, got);
         sceIoClose(f);
         return -1;
      }
   }
   else
   {
      for (y = 0; y < h; y++)
         if (sceIoRead(f, tex + (size_t)y * stride, fstride * 2)
             != (int)fstride * 2)
         {
            sceIoClose(f);
            return -1;
         }
   }
   sceIoClose(f);
   *dw = w;
   *dh = h;
   return 0;
}

/* Write what we just decoded, so the next visit is a single read.  Best
 * effort: a full or read-only card simply means we decode again next time,
 * which is exactly today's behaviour and not worth reporting as a failure. */
static void art_cache_565(const char *path, const uint16_t *tex, int stride,
                          int w, int h)
{
   unsigned char hdr[R565_HDR];
   SceUID f;
   int y;

   if (w <= 0 || h <= 0)
      return;
   f = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (f < 0)
      return;
   memset(hdr, 0, sizeof(hdr));
   hdr[0] = '5'; hdr[1] = '6'; hdr[2] = '5'; hdr[3] = 'R';
   hdr[4] = (unsigned char)(w & 0xFF);  hdr[5] = (unsigned char)(w >> 8);
   hdr[6] = (unsigned char)(h & 0xFF);  hdr[7] = (unsigned char)(h >> 8);
   hdr[8] = (unsigned char)(stride & 0xFF);
   hdr[9] = (unsigned char)(stride >> 8);
   if (sceIoWrite(f, hdr, R565_HDR) == R565_HDR)
      for (y = 0; y < h; y++)
         if (sceIoWrite(f, tex + (size_t)y * stride, stride * 2) != stride * 2)
            break;
   sceIoClose(f);
   fe_evt("art_cached path=%s %dx%d", path, w, h);
}

/* Try png, jpg, bmp in that order.  Returns 0 on success;
 * dw and dh come in as the box to fit and come back as the sub-rect
 * actually written. */
/* `base` is the ROM's file name (no folder): its stem names the art. */
static int art_load_any(const char *base, const char *dir, uint16_t *tex,
                        int stride, int *dw, int *dh)
{
   extern char g_dir_base[];
   /* .565 first: it is the texture itself and costs one read. */
   static const char *ext[] = { "565", "png", "jpg", "jpeg", "bmp" };
   int stem = (int)ui_rom_stem_length(base);
   unsigned e;

   int boxw = *dw, boxh = *dh;
   unsigned t0 = (unsigned)sceKernelGetSystemTimeLow();

   FE_EVT_ONLY(t0);

   for (e = 0; e < sizeof(ext) / sizeof(ext[0]); e++)
   {
      char path[PSP_FILE_PATH_CAP];
      SceIoStat st;
      int path_len;
      /* A failed attempt may have shrunk the box; every candidate gets
       * the same frame to fit into. */
      *dw = boxw; *dh = boxh;
      path_len = snprintf(path, sizeof(path), "%s/%s/%.*s.%s", g_dir_base,
                          dir, stem, base, ext[e]);
      if (path_len < 0 || (size_t)path_len >= sizeof(path))
         continue;
      if (sceIoGetstat(path, &st) < 0)
         continue;
      if (ext[e][0] == '5')
      {
         if (art_load_565(path, tex, stride, dw, dh) == 0)
         {
            fe_evt("art_time fmt=565 us=%u %s",
                   (unsigned)sceKernelGetSystemTimeLow() - t0, path);
            return 0;
         }
      }
      else if (ext[e][0] == 'b')
      {
         if (art_load_bmp(path, tex, stride, dw, dh) == 0)
         {
            fe_evt("art_time fmt=bmp us=%u %s",
                   (unsigned)sceKernelGetSystemTimeLow() - t0, path);
            return 0;
         }
      }
      else if (art_load_stb(path, tex, stride, dw, dh) == 0)
      {
         /* Decoded the slow way -- leave a .565 behind so this title is a
          * single read from now on.  Lazy, one title at a time, only for
          * something the user actually looked at: there is deliberately no
          * batch pass, because converting a 100-ROM library at boot would
          * make the emulator look hung on first run. */
         char cpath[PSP_FILE_PATH_CAP];
         int cache_len = snprintf(cpath, sizeof(cpath), "%s/%s/%.*s.565",
                                  g_dir_base, dir, stem, base);
         if (cache_len < 0 || (size_t)cache_len >= sizeof(cpath))
            return 0; /* art decoded, but its cache destination cannot fit */
         fe_evt("art_time fmt=png us=%u %s",
                (unsigned)sceKernelGetSystemTimeLow() - t0, path);
         art_cache_565(cpath, tex, stride, *dw, *dh);
         return 0;
      }
   }
   return -1;
}

/* Cache lookup; loads on miss (`load` != 0).  Returns the texture or NULL.
 * A negative result is cached too -- retrying a missing file every frame
 * would be 60 directory misses a second on a memory stick that ADR-0069
 * priced at 12.4 ms per negative lookup. */
static const uint16_t *art_get(const char *rom_dir, int rom_idx, int load,
                               art_slot *out)
{
   int i, victim;
   (void)rom_dir;

   for (i = 0; i < ART_SLOTS; i++)
      if (g_art[i].state && g_art[i].rom_idx == rom_idx)
      {
         if (out)
            *out = g_art[i];
         return g_art[i].state > 0 ? g_art[i].tex : NULL;
      }
   if (!load)
      return NULL;

   victim = g_art_clock;
   g_art_clock = (g_art_clock + 1) % ART_SLOTS;
   if (!g_art[victim].tex)
   {
      g_art[victim].tex = (uint16_t *)memalign(16, ART_TEX_W * ART_TEX_H * 2);
      if (!g_art[victim].tex)
         return NULL;
   }
   memset(g_art[victim].tex, 0, ART_TEX_W * ART_TEX_H * 2);
   g_art[victim].rom_idx = rom_idx;
   g_art[victim].aw = ART_W;
   g_art[victim].ah = ART_H;
   if (art_load_any(g_roms[rom_idx].base, "boxart", g_art[victim].tex,
                    ART_TEX_W,
                    &g_art[victim].aw, &g_art[victim].ah) == 0)
   {
      sceKernelDcacheWritebackRange(g_art[victim].tex,
                                    ART_TEX_W * ART_TEX_H * 2);
      g_art[victim].state = 1;
      if (out)
         *out = g_art[victim];
      return g_art[victim].tex;
   }
   g_art[victim].state = -1;
   return NULL;
}

/* The marquee's full-bleed background.  One texture, rebuilt only when the
 * selection changes AND the stick has been idle -- a hero decode is the most
 * expensive thing the browser does, and doing it mid-scroll would stutter
 * exactly when the user is moving. */
static const uint16_t *hero_decode(int rom_idx, const char *base);

static const uint16_t *hero_get(int rom_idx, int load)
{
   if (g_hero_idx == rom_idx && g_hero_state)
      return g_hero_state > 0 ? g_hero : NULL;
   if (!load)
      return g_hero_state > 0 && g_hero_idx == rom_idx ? g_hero : NULL;
   return hero_decode(rom_idx, g_roms[rom_idx].base);
}

/* The decode itself, by file name, so a launch that never saw the browser
 * (harness, variant) can fetch the same art.  `rom_idx` only keys the
 * one-entry cache; HERO_IDX_PATH marks a by-path load. */
#define HERO_IDX_PATH (-2)
static const uint16_t *hero_decode(int rom_idx, const char *base)
{

   if (!g_hero)
   {
      /* Native first, half size second, no backdrop last.  Every step down is
       * a worse picture but never an unstable one -- the shell reads fine
       * with no hero at all. */
      static const int tries[] = { HERO_TEX_BIG, HERO_TEX_SMALL };
      unsigned t;
      for (t = 0; t < sizeof(tries) / sizeof(tries[0]); t++)
      {
         unsigned bytes = (unsigned)tries[t] * tries[t] * 2;
         g_hero = (uint16_t *)memalign(16, bytes);
         if (g_hero)
         {
            g_hero_tex = tries[t];
            break;
         }
      }
      if (!g_hero)
      {
         g_hero_state = -1;
         g_hero_idx = rom_idx;
         return NULL;
      }
      fe_evt("hero_tex edge=%d bytes=%u", g_hero_tex,
             (unsigned)g_hero_tex * g_hero_tex * 2);
   }
   memset(g_hero, 0, (size_t)g_hero_tex * g_hero_tex * 2);
   g_hero_idx = rom_idx;
   g_hero_precomposed = 0;

   /* hero/<rom>.png first: purpose-made 16:9 art.  It is fitted to the FULL
    * texture rather than the 5:7 box the cover path uses, because it is
    * already the right shape -- passing it the portrait box would letterbox
    * a widescreen image inside a widescreen frame. */
   /* At 512 this is the screen itself, so the draw below is 1:1. */
   g_hero_w = (g_hero_tex >= 480) ? 480 : g_hero_tex;
   g_hero_h = (g_hero_w * 272) / 480;
   if (art_load_any(base, "hero", g_hero, g_hero_tex,
                    &g_hero_w, &g_hero_h) == 0)
   {
      sceKernelDcacheWritebackRange(g_hero,
                                    (size_t)g_hero_tex * g_hero_tex * 2);
      g_hero_state = 1;
      g_hero_precomposed = 1;
      return g_hero;
   }

   /* Otherwise derive one from the cover, as before.  The 5:7 cover box is
    * 256x358, but the texture can be the 256x256 fallback (HERO_TEX_SMALL,
    * when the 512 KiB one was refused -- most likely on a PSP-1000): a tall
    * cover fitted into the full box wrote up to 102 rows (~52 KB) past the
    * end of the buffer.  Clamp the box to the texture actually held; the fit
    * keeps the aspect, so a tall cover just comes out smaller. */
   g_hero_w = HERO_W < g_hero_tex ? HERO_W : g_hero_tex;
   g_hero_h = HERO_H < g_hero_tex ? HERO_H : g_hero_tex;
   if (art_load_any(base, "boxart", g_hero, g_hero_tex,
                    &g_hero_w, &g_hero_h) == 0)
   {
      sceKernelDcacheWritebackRange(g_hero,
                                    (size_t)g_hero_tex * g_hero_tex * 2);
      g_hero_state = 1;
      return g_hero;
   }
   g_hero_state = -1;
   return NULL;
}

/* Display name: extension already stripped by the caller; additionally cut
 * region tags for the card face ("Pokemon - Emerald Version (USA, Europe)"
 * -> "Pokemon - Emerald Version"). */
static void rom_display_name(const rom_entry *r, char *out, size_t sz,
                             int cut_region)
{
   size_t n = ui_rom_stem_length(r->base);
   if (n >= sz)
      n = sz - 1;
   memcpy(out, r->base, n);
   out[n] = '\0';
   /* No-Intro writes "Legend of Zelda, The".  Restoring the article to the
    * front costs nothing, reads properly, and matters most on exactly the
    * titles long enough to be truncated. */
   {
      char *c = strstr(out, ", The");
      if (c && (c[5] == '\0' || c[5] == ' ' || c[5] == '-'))
      {
         /* Done IN PLACE.  Moving ", The" to the front takes five characters
          * out and puts four back, so the result is always one byte shorter
          * than what is already in `out` -- which means it always fits, and
          * the scratch buffer this used to need (whose size had to be guessed
          * against the caller's `sz`, and was the only truncation risk in the
          * function) is not needed at all.
          *
          * Order matters and is safe: the shift right by 4 writes no further
          * than out[head+3], which is before the ", The" at out[head+5], so
          * the tail is still intact when it is moved left by one. */
         size_t head = (size_t)(c - out);
         size_t tail = strlen(c + 5);
         memmove(out + 4, out, head);
         memmove(out + 4 + head, out + head + 5, tail + 1);
         memcpy(out, "The ", 4);
      }
   }
   if (cut_region)
   {
      char *par = strchr(out, '(');
      while (par > out && par[-1] == ' ')
         par--;
      if (par && par > out)
         *par = '\0';
   }
}

/* Template card for a ROM with no art: accent header strip, wrapped title,
 * GBA badge.  Deliberately styled so a folder with zero art still looks
 * designed rather than broken. */
static void card_template(int x, int y, int w, int h, const rom_entry *r)
{
   char title[64];
   int max_cols = (w - 16) / (FE_FONT_W - 1);
   int max_rows = (h - 40) / (FE_FONT_H + 1);
   int line = 0;
   const char *p;

   vid_rect(x, y, w, h, C_CARD, 255);
   vid_rect(x, y, w, 4, C_ACCENT, 255);
   rom_display_name(r, title, sizeof(title), 1);

   p = title;
   while (*p && line < max_rows)
   {
      char seg[40];
      int n = (int)strlen(p);
      if (n > max_cols)
      {
         /* break at the last space that fits, else hard-break */
         int b = max_cols;
         while (b > 0 && p[b] != ' ')
            b--;
         n = b > 0 ? b : max_cols;
      }
      if (n >= (int)sizeof(seg))
         n = (int)sizeof(seg) - 1;
      memcpy(seg, p, n);
      seg[n] = '\0';
      vid_text(x + 8, y + 14 + line * (FE_FONT_H + 1), seg, C_ITEM);
      p += n;
      while (*p == ' ')
         p++;
      line++;
   }
   vid_text(x + w - 8 - vid_text_w(skin_of(g_pcfg.console)->id),
            y + h - FE_FONT_H - 6, skin_of(g_pcfg.console)->id, C_ACCENT_DK);
}

/* ============================ SHELLS =====================================
 *
 * SHELL A ("Shelf")   -- default. Vertical list left, one art panel right.
 * SHELL B ("Marquee") -- the selected art fills the screen, dimmed, with the
 *                        list floating over it.
 *
 * Both draw with the primitives the old coverflow already used: solid rects,
 * text and one vid_image per art. No new engine, no scaler.  `ui_shell` in
 * config.ini picks between them and needs a restart, matching how Media
 * Engine mode is toggled.
 *
 * Why vertical: the selection sits at a FIXED row and the list slides
 * underneath it, so the eye never chases the cursor. The old coverflow moved
 * five cards at once and drew four of them only to throw a dim veil over
 * them -- four art fetches per frame spent on decoration, which is the other
 * half of why art was perpetually loading.
 *
 * g_ui_shell (0 = Shelf, 1 = Marquee) is defined with the browser state
 * above: the header cluster, drawn by both shells, reads it. */

/* ---- motion -------------------------------------------------------------
 *
 * The list used to snap: rows were positioned straight from the index, so a
 * d-pad press teleported the whole column by one row height.  Now `g_scroll`
 * chases the selection and the rows are drawn from IT, so the list slides.
 *
 * Fixed point, 8 fractional bits.  There is no FPU cost worth avoiding here
 * -- the browser draws a dozen primitives at 60 Hz -- but integer maths keeps
 * the row positions exactly reproducible, which matters because the harness
 * screenshots the gallery and compares it.
 *
 * ~120 ms to settle.  Faster reads as a snap with a smear on it; slower
 * fights you when the d-pad is held down, because the list is still moving
 * when the next repeat arrives.  Exponential rather than linear so it leaves
 * immediately (responsive) and arrives gently (finished).
 *
 * WRAPPING IS THE TRAP.  The list is a carousel: going from the last entry to
 * the first changes `cur` by -(n-1), and easing across that gap sends the
 * column flying through the entire library.  Any step of more than half the
 * list is a wrap, and a wrap is the one case that must NOT animate. */
#define SCROLL_FP     8                     /* fractional bits            */
#define SCROLL_ONE    (1 << SCROLL_FP)
#define SCROLL_DUR    8                     /* frames per move -> ~133 ms */

static int g_scroll;                        /* current, in FP rows        */
static int g_scroll_from, g_scroll_to;      /* tween endpoints            */
static int g_scroll_t;                      /* frames elapsed, 0..DUR     */
static int g_scroll_cur = -1;               /* selection g_scroll_to is for */

/* Smoothstep, u and result both 0..256.  u*u*(3-2u) in fixed point. */
static int smoothstep(int u)
{
   return (((u * u) >> SCROLL_FP) * (3 * SCROLL_ONE - 2 * u)) >> SCROLL_FP;
}

/* A FIXED-DURATION TWEEN, NOT EXPONENTIAL DECAY.
 *
 * The first version chased the target by a fraction of the remaining
 * distance each frame.  That is the textbook approach and it feels wrong,
 * because the pixel steps it produces on a 25 px row are
 *     10, 6, 4, 2, 1, 1, 1, 0, 0, 0
 * -- a lurch, then a crawl, then several frames where nothing moves at all.
 * Reported as "a bit stuttery", and the numbers agree.
 *
 * Smoothstep over a fixed 8 frames gives
 *      1, 2, 4, 5, 5, 4, 2, 2
 * which accelerates and decelerates symmetrically and never stalls.
 *
 * Re-targeting mid-tween starts a fresh tween FROM THE CURRENT POSITION, so
 * holding the d-pad keeps the column gliding instead of restarting it. */
static void scroll_track(int cur, int n)
{
   if (g_scroll_cur < 0)                    /* first frame: no animation  */
   {
      g_scroll_cur = cur;
      g_scroll = g_scroll_from = g_scroll_to = cur * SCROLL_ONE;
      g_scroll_t = SCROLL_DUR;
      return;
   }
   if (cur != g_scroll_cur)
   {
      int step = cur - g_scroll_cur;
      g_scroll_cur = cur;
      g_scroll_from = g_scroll;             /* glide on from here         */
      g_scroll_to   = cur * SCROLL_ONE;
      g_scroll_t    = 0;
      /* A wrap is a jump of more than half the library; snap through it
       * rather than scrolling the whole way round. */
      if (n > 2 && (step > n / 2 || step < -(n / 2)))
      {
         g_scroll = g_scroll_from = g_scroll_to;
         g_scroll_t = SCROLL_DUR;
      }
   }
   if (g_scroll_t < SCROLL_DUR)
   {
      int e;
      g_scroll_t++;
      e = smoothstep((g_scroll_t * SCROLL_ONE) / SCROLL_DUR);
      g_scroll = g_scroll_from +
                 (((g_scroll_to - g_scroll_from) * e) >> SCROLL_FP);
   }
   else
      g_scroll = g_scroll_to;
}

/* True once the tween has finished.  DECODING IS GATED ON THIS.
 *
 * Nothing in this browser can decode a PNG inside a frame budget -- a 512
 * square cover takes hundreds of milliseconds on Allegrex -- so a decode
 * started while the list is moving stops the list dead until it finishes.
 * The original gate was `idle >= 2`, i.e. 33 ms after the last press, while
 * d-pad repeat arrives every ~100 ms.  Every repeat therefore kicked off a
 * decode, which is why holding a direction hitched and sometimes stalled
 * long enough to draw a whole cover before moving on.
 *
 * The rule is simply: never decode while anything is animating, and not
 * until the user has actually stopped. */
static int scroll_settled(void)
{
   return g_scroll_t >= SCROLL_DUR && g_scroll == g_scroll_to;
}

/* Frames of stillness before each kind of decode is allowed.  The selected
 * cover is what the user is waiting for, so it goes first; neighbours are
 * speculative and wait until the browser is plainly idle. */
#define IDLE_SELECTED  10        /* ~170 ms */
#define IDLE_HERO      14        /* ~230 ms */
#define IDLE_PREFETCH  24        /* ~400 ms */

/* Pixels the column is offset by, relative to the selected row's slot. */
static int scroll_px(int cur, int row_h)
{
   return ((cur * SCROLL_ONE - g_scroll) * row_h) >> SCROLL_FP;
}

/* ---- art fade ------------------------------------------------------------
 * A decoded cover used to appear between one frame and the next.  Ramping it
 * in over ~8 frames also disguises the decode latency, which is the same
 * event seen from the other side. */
#define ART_FADE_STEP 32

static int  g_art_fade;                     /* 0..255 for the current art */
static int  g_art_fade_idx = -1;

static int art_alpha(int rom_idx, const void *art)
{
   if (rom_idx != g_art_fade_idx)
   {
      g_art_fade_idx = rom_idx;
      g_art_fade = 0;
   }
   if (!art)
      return 0;
   g_art_fade += ART_FADE_STEP;
   if (g_art_fade > 255)
      g_art_fade = 255;
   return g_art_fade;
}

#define SHELF_ROWS   7                    /* visible list rows */
#define SHELF_SEL    3                    /* the fixed cursor row */
#define SHELF_ROW_H  25
#define SHELF_TOP    48
#define ART_X        (480 - 20 - ART_W)
#define ART_Y        40
#define SHELF_CARD_H 152                 /* the no-art placeholder card */

/* Warm the cache around the cursor so scrolling never waits on a decode.
 * Deliberately loads at most ONE per call: a burst of six decodes would
 * stutter worse than the pop-in it is meant to cure. */
static void art_prefetch(const char *rom_dir, int cur, int n)
{
   static const int off[] = { 0, 1, -1, 2, -2, 3, -3 };
   unsigned k;
   for (k = 0; k < sizeof(off) / sizeof(off[0]); k++)
   {
      /* `cur` and `n` are list rows; the cache is keyed by ROM. */
      int idx = view_rom(((cur + off[k]) % n + n) % n);
      int j, have = 0;
      for (j = 0; j < ART_SLOTS; j++)
         if (g_art[j].rom_idx == idx && g_art[j].state != 0)
            have = 1;
      if (!have)
      {
         art_get(rom_dir, idx, 1, NULL);  /* decode exactly one, then stop */
         return;
      }
   }
}

static void meta_line(const rom_entry *r, char *out, size_t sz)
{
   unsigned mb = (unsigned)((r->size + (1u << 19)) >> 20);
   snprintf(out, sz, "%u MB", mb ? mb : 1u);
}

/* Trim to a PIXEL budget: the face is proportional, so a column count
 * clips "WWWW" and "iiii" at wildly different widths. */
static void clip_title(char *t, int px)
{
   int n = (int)strlen(t);
   if (vid_text_w(t) <= px)
      return;
   /* Leave room for the ellipsis, then trim until the whole thing fits.
    * A hard cut is ambiguous -- "Pokemon Mystery Dungeon - Red Rescue Tea"
    * reads as a title someone typed wrong.  Three dots say "there is more"
    * and cost eleven pixels. */
   while (n > 1)
   {
      t[--n] = 0;
      if (vid_text_w(t) + vid_text_w("...") <= px)
         break;
   }
   /* Do not leave a dangling space or separator before the dots. */
   while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '-'))
      t[--n] = 0;
   strcpy(t + n, "...");
}

/* Loading stays on the main thread, like every other GU user. Progress is
 * driven by completed reads, not a second thread racing the loader/renderer.
 * Keep the existing large reads; throttle redraws rather than splitting I/O. */
static struct {
   int active, count;
   char title[132]; /* leave room for clip_title's ellipsis */
   unsigned started, stage_at, drawn_at;
   const char *stage[8];
   unsigned elapsed[8];
} g_loading;

/* ---- LAUNCH ART (docs/DISPLAY-FEATURES.md) --------------------------------
 *
 * The game's hero art, taken ONCE when a ROM is picked, becomes the ambient
 * texture (32 KiB of VRAM, ambient_look.h) -- and that same bake is the
 * loading screen's background (Fable's "L2").  Source: the hero the browser
 * already decoded when it is this ROM's (the Marquee shell usually has it);
 * otherwise the Marquee's own decode, by file name, into the same buffer.
 * Then the browser frees its whole art pool exactly as it always did --
 * before fe_host_boot, so the core's startup allocations (residency, JIT
 * tier, the ROM-cache loop) find the heap as they did in 3.1.  The sharp
 * hero is never kept ("L1" would hold 512 KiB through the load).
 *
 * Every step is optional: no art, no VRAM room, or `loading_art = 0` gives
 * the palette loading screen and palette/black bars. */
static int g_launch_taken;       /* a launch has been through here        */
static int g_loading_dumps;      /* harness `loading_dump = N`             */
void ui_loading_dump_shots(int n) { g_loading_dumps = n > 0 ? n : 0; }

static void browser_cart(int cx, int cy, const ui_skin *s);

/* The ambient bars' no-art colour: the console's darkest palette shade. */
uint16_t ui_console_shade(int console)
{
   if (console < FE_CONSOLE_GBA || console >= FE_CONSOLE_COUNT)
      console = FE_CONSOLE_GBA;
   return SKIN_DARK[console].pal[0];
}

static void launch_art_take(void)
{
   int rows;
   if (g_hero_state <= 0 || !g_hero || g_hero_w <= 0 || g_hero_h <= 0)
      return;
   /* The texture is g_hero_tex square: never read a row past it. */
   rows = g_hero_h < g_hero_tex ? g_hero_h : g_hero_tex;
   vid_ambient_bake_art(g_hero, g_hero_tex, g_hero_w, rows,
                        g_hero_precomposed);
}

/* Browser pick: reuse the decoded hero when it is this ROM's. */
static void launch_art_from_browser(int rom)
{
   unsigned t0 = sceKernelGetSystemTimeLow();
   int reused = (g_hero_idx == rom && g_hero_state > 0);
   FE_EVT_ONLY(t0);
   g_launch_taken = 1;
   if (!g_pcfg.loading_art)
      return;
   if (!reused)
      hero_get(rom, 1);   /* the Marquee's decode; art_free_all() follows */
   if (g_hero_idx == rom && g_hero_state > 0)
      launch_art_take();
   fe_evt("launch_art_source rom=%s reused=%d found=%d us=%u", g_roms[rom].name,
          reused, vid_ambient_source() == 1,
          (unsigned)sceKernelGetSystemTimeLow() - t0);
}

/* Harness / variant launch: no browser, so decode by the ROM's file name,
 * bake, and free the decode at once -- still before fe_host_boot. */
static void launch_art_from_path(const char *path)
{
   const char *base = strrchr(path, '/');
   unsigned t0 = sceKernelGetSystemTimeLow();
   FE_EVT_ONLY(t0);
   g_launch_taken = 1;
   if (!g_pcfg.loading_art)
      return;
   base = base ? base + 1 : path;
   if (hero_decode(HERO_IDX_PATH, base))
      launch_art_take();
   art_free_all();
   fe_evt("launch_art_source path=%s reused=0 found=%d us=%u", base,
          vid_ambient_source() == 1,
          (unsigned)sceKernelGetSystemTimeLow() - t0);
}

/* clip_title, measured in the face the loading title is drawn in. */
static void clip_title_hd(char *t, int px)
{
   int n = (int)strlen(t);
   if (vid_text_hd_w(t) <= px)
      return;
   while (n > 1)
   {
      t[--n] = 0;
      if (vid_text_hd_w(t) + vid_text_hd_w("...") <= px)
         break;
   }
   while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '-'))
      t[--n] = 0;
   strcpy(t + n, "...");
}

/* "L2": the browser's marquee chrome over the ambient bake, or over the
 * console's palette ramp when there is no art (Fable, loading/). */
void ui_loading_update(const char *stage, unsigned done, unsigned total)
{
   unsigned now = sceKernelGetSystemTimeLow();
   int changed;
   char amount[48];
   const ui_skin *s;
   if (!g_loading.active) return;
   changed = !g_loading.count ||
             strcmp(stage, g_loading.stage[g_loading.count - 1]) != 0;
   if (changed)
   {
      if (g_loading.count)
         g_loading.elapsed[g_loading.count - 1] += now - g_loading.stage_at;
      if (g_loading.count < 8)
         g_loading.stage[g_loading.count++] = stage;
      g_loading.stage_at = now;
   }
   if (!changed && now - g_loading.drawn_at < 100000 && done != total)
      return;
   g_loading.drawn_at = now;
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;
   s = skin_of(g_pcfg.console);
   vid_overlay_begin(1);

   /* Background. */
   vid_rect(0, 0, VID_SCR_W, VID_SCR_H,
            g_theme_black ? 0x0000 : C_BG_TOP, 255);
   if (vid_ambient_image(0, 0, VID_SCR_W, VID_SCR_H, 255))
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, C_BG_TOP, LOAD_SCRIM);
   else
   {
      vid_gradient_a(0, 0, VID_SCR_W, VID_SCR_H, s->pal[0],
                     g_pcfg.theme == 1 ? LOAD_PAL_RAMP_LIGHT
                                       : LOAD_PAL_RAMP_DARK, 0);
      browser_cart(240, 180, s);
   }
   vid_gradient_a(0, 232, VID_SCR_W, 40, C_BG_TOP, 0, LOAD_FOOT_RAMP);

   /* Chrome: wordmark, console card, the busy ring -- the browser's header
    * row (HDR_*), so nothing in it moves when a game is picked. */
   vid_logo(HDR_LOGO_X, HDR_LOGO_Y, C_ACCENT, 255);
   hdr_ring(hdr_card(s, 256), now, C_TITLE);

   /* Title, stage, amount, progress in the console's colour. */
   vid_text_hd(20, 74, g_loading.title, C_SEL);
   vid_text(20, 102, stage, C_ITEM);
   if (total)
   {
      if (done > total) done = total;
      snprintf(amount, sizeof(amount), "%u / %u KiB", done / 1024,
               (total + 1023) / 1024);
      vid_text(460 - vid_text_w(amount), 102, amount, C_DIM);
      vid_rect(20, 126, 440, 3, C_ACCENT_DK, 160);
      vid_rect(20, 126, (int)((unsigned long long)done * 440 / total), 3,
               s->accent, 255);
   }
   vid_overlay_end();
   if (g_loading_dumps > 0)
   {
      /* Harness: the loading screen is never on screen long under PPSSPP,
       * and neither browser nor game loop runs while it is. */
      extern char g_dir_base[];
      char gp[176];
      static int n;
      g_loading_dumps--;
      snprintf(gp, sizeof(gp), "%s/log/ge_loading_%d.bmp", g_dir_base, n++);
      if (vid_dump_ge(gp) == 0)
         fe_evt("ge_dump file=ge_loading_%d.bmp ui=1", n - 1);
   }
   vid_swap();
   /* Ensure the status is actually visible BEFORE the next blocking call.
    * The existing swap path still owns all display-buffer safety. */
   sceDisplayWaitVblankStart();
}

void ui_loading_begin(const char *path)
{
   const char *name = strrchr(path, '/');
   char *dot;
   if (g_loading.active) return; /* browser already started the timer */
   g_ret_on = 0;   /* a launch with no browser: no crossfade to finish */
   /* No browser pick came first (harness or variant launch): fetch the
    * art by file name, before the core's allocations. */
   if (!g_launch_taken)
      launch_art_from_path(path);
   memset(&g_loading, 0, sizeof(g_loading));
   g_loading.active = 1;
   g_loading.started = sceKernelGetSystemTimeLow();
   snprintf(g_loading.title, sizeof(g_loading.title) - 4, "%s", name ? name + 1 : path);
   dot = strrchr(g_loading.title, '.');
   if (dot) *dot = 0;
   clip_title_hd(g_loading.title, 440);
   ui_loading_update("Remembering game", 0, 0);
}

/* A full-screen notice for a boot that cannot continue (docs/MEMORY-BUDGET.md:
 * the memory gate; a save that could not be read).  Held until a button is
 * pressed or hold_ms passes, so an unattended rig still returns to the XMB. */
void ui_notice(const char *title, const char *l1, const char *l2,
               const char *l3, unsigned hold_ms)
{
   unsigned t0 = sceKernelGetSystemTimeLow();
   SceCtrlData pad;
   g_loading.active = 0;
   do
   {
      vid_overlay_begin(1);
      page(title, NULL);
      if (l1) vid_text_center(96, l1, C_WARN);
      if (l2) vid_text_center(124, l2, C_ITEM);
      if (l3) vid_text_center(152, l3, C_ITEM);
      vid_text_center(200, "Press any button to return to the XMB.", C_DIM);
      vid_overlay_end();
      vid_swap();
      sceDisplayWaitVblankStart();
      memset(&pad, 0, sizeof(pad));
      sceCtrlPeekBufferPositive(&pad, 1);
      if ((pad.Buttons & 0xF3F9u) &&
          sceKernelGetSystemTimeLow() - t0 > 500000u)   /* not a held key */
         break;
   } while (sceKernelGetSystemTimeLow() - t0 < hold_ms * 1000u);
}

void ui_loading_finish(int success)
{
   unsigned now = sceKernelGetSystemTimeLow();
   if (!g_loading.active) return;
   if (g_loading.count)
      g_loading.elapsed[g_loading.count - 1] += now - g_loading.stage_at;
   g_loading.active = 0;
#ifdef GPSP_ROMLOAD_DIAGNOSTICS
   /* Test builds only: one small write AFTER timing, no per-read logging.
    * This measures the real storage/CPU, which PPSSPP cannot price. */
   {
      extern char g_dir_base[];
      char path[256];
      FILE *f;
      int i;
      snprintf(path, sizeof(path), "%s/ROMLOAD.TXT", g_dir_base);
      f = fopen(path, "w");
      if (f)
      {
         fprintf(f, "result=%s total_us=%u\n", success ? "ok" : "failed",
                  now - g_loading.started);
         for (i = 0; i < g_loading.count; i++)
            fprintf(f, "%s: %u us\n", g_loading.stage[i], g_loading.elapsed[i]);
         fclose(f);
      }
   }
#else
   (void)success;
#endif
}

/* HORIZONTAL SCROLL FOR THE SELECTED TITLE.
 *
 * Truncation is honest but it still hides the end of a long name, and the
 * selected row is the one the user is actually reading.  So that one -- and
 * only that one, because more than one moving thing is noise -- slides left
 * to reveal the rest, then returns.
 *
 * Dwell at each end: motion that starts the instant you arrive is unreadable,
 * and motion that never rests is exhausting.  ~1.3 s still, then ~30 px/s,
 * then still again before it snaps back. */
#define MARQ_DWELL   80          /* frames held at each end (~1.3 s)      */
#define MARQ_SPEED   2           /* frames per pixel travelled            */

static int g_marq_t;
static int g_marq_idx = -1;

/* Pixels to shift the selected title left; 0 when it fits or is resting. */
static int marquee_offset(int rom_idx, const char *full, int px)
{
   /* MEASURE WITH THE FACE IT IS DRAWN IN.  This asked vid_text_w (Regular
    * 15 px) about a string rendered with vid_text_hd (SemiBold 19 px), so
    * anything that overflowed at 19 but fitted at 15 never scrolled -- which
    * is most of them.  The ones that did scroll also stopped short, because
    * `over` was computed from the narrower face. */
   int over = vid_text_hd_w(full) - px;
   int travel, cycle, t;

   if (rom_idx != g_marq_idx)
   {
      g_marq_idx = rom_idx;
      g_marq_t = 0;
   }
   if (over <= 0)
      return 0;                      /* fits: never moves */
   g_marq_t++;
   travel = over * MARQ_SPEED;
   cycle  = MARQ_DWELL + travel + MARQ_DWELL;
   t = g_marq_t % cycle;
   if (t < MARQ_DWELL)
      return 0;
   if (t < MARQ_DWELL + travel)
      return (t - MARQ_DWELL) / MARQ_SPEED;
   return over;
}

/* ---- SHELL A: Shelf ---------------------------------------------------- */
/* `n` is the rows in the current view (favourites or all); `n_scan` is the
 * scan, for the games count. */
static void shell_shelf(const char *rom_dir, int cur, int n, int n_scan,
                        int idle)
{
   const uint16_t *art;
   char buf[64], title[64];
   int i, sy;

   /* APPLY THE THEME.  g_thm was only ever assigned inside page(), which
    * the browser does not call -- so the gallery rendered with whatever
    * palette the last menu screen happened to leave behind.  Boot went
    * to the browser dark; opening Settings and coming back made it
    * light.  Set it where the drawing happens. */
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;

   scroll_track(cur, n);

   vid_rect(0, 0, 480, 272, C_BG_TOP, 255);

   vid_logo(HDR_LOGO_X, HDR_LOGO_Y, C_ACCENT, 255);
   browser_header(n_scan);

   /* The selection band does NOT move -- it is the fixed thing the list
    * slides under.  Only the rows take the scroll offset.  Its edge is the
    * console colour: one of the three places that colour is spent. */
   /* Cap height centred on the selected row's text (vid_band_y): 7 px of
    * card above the capitals and 7 below.  It was `- 4`: 8 above, 6 below. */
   vid_rect(0, vid_band_y(SHELF_TOP + SHELF_SEL * SHELF_ROW_H, SHELF_ROW_H),
            300, SHELF_ROW_H, C_CARD, 255);
   vid_rect(0, vid_band_y(SHELF_TOP + SHELF_SEL * SHELF_ROW_H, SHELF_ROW_H),
            3, SHELF_ROW_H, C_CON, 255);

   sy = scroll_px(cur, SHELF_ROW_H) + g_fx_dy;

   /* CLIP THE LIST.  A row sliding in from above used to be drawn wherever
    * the tween put it, which with the wordmark now occupying y=8..30 meant
    * the incoming title crossed the logo.  Clipping cuts it at the boundary
    * instead, so the column slides UNDER the header the way it should. */
   vid_clip(0, SHELF_TOP - 6, 300, 246 - (SHELF_TOP - 6));

   /* One row of overscan each side, so a row sliding in is drawn before it
    * reaches the visible band instead of appearing at the edge. */
   for (i = -1; i <= SHELF_ROWS; i++)
   {
      int rel = i - SHELF_SEL;
      int idx = view_rom(((cur + rel) % n + n) % n);
      int y   = SHELF_TOP + i * SHELF_ROW_H + sy;
      int d   = rel < 0 ? -rel : rel;
      int dx, fade;
      uint16_t col = (d == 0) ? C_SEL : (d == 1) ? C_ITEM : C_DIM;

      if (y < SHELF_TOP - SHELF_ROW_H || y > 240)
         continue;

      /* Short libraries do not wrap: padding a 3-game list with repeats
       * reads as a bug, not as a carousel. */
      if (n < SHELF_ROWS && (cur + rel < 0 || cur + rel >= n))
         continue;

      row_fx(rel, &dx, &fade);
      col = mix565(col, C_BG_TOP, fade);
      rom_display_name(&g_roms[idx], title, sizeof(title), 1);
      /* 208, not 232: the star column sits between the title and SAVE. */
      clip_title(title, 208);
      vid_text(20 + dx, y, title, col);
      if (g_roms[idx].is_fav && !(g_pop.active && g_pop.view == cur + rel))
         vid_text(234 + dx, y, VID_GLYPH_STAR,
                  mix565(d == 0 ? C_CON : mix565(C_CON, C_BG_TOP, 115),
                         C_BG_TOP, fade));
      if (rom_has_sav(idx))
         vid_text(288 - vid_text_w("SAVE") + dx, y, "SAVE",
                  mix565(d == 0 ? C_ACCENT : C_ACCENT_DK, C_BG_TOP, fade));
   }
   vid_clip_off();
   browser_pop_draw(234, SHELF_TOP + SHELF_SEL * SHELF_ROW_H + sy,
                    SHELF_ROW_H, cur);

   {
      /* The cover is TOP-anchored in the panel and the caption follows its
       * real bottom edge, not the frame's.  Centring instead left a square
       * libretro boxart floating with dead space under it and pushed the
       * caption into the footer. */
      art_slot a;
      int ax = ART_X, ay = ART_Y, bot, rom = view_rom(cur);
      a.aw = ART_W; a.ah = ART_H;
      art = art_get(rom_dir, rom, idle >= IDLE_SELECTED && scroll_settled(), &a);
      if (art)
      {
         int al = (art_alpha(rom, art) * (256 - g_fx_fade)) >> 8;
         ax = ART_X + (ART_W - a.aw) / 2;
         vid_rect(ax + 3, ay + 4, a.aw, a.ah, C_SHADOW, (90 * al) / 255);
         vid_image(ax, ay, a.aw, a.ah, art,
                   ART_TEX_W, ART_TEX_H, a.aw, a.ah, al);
         bot = ay + a.ah;
      }
      else
      {
         art_alpha(rom, NULL);          /* arm the fade for when it lands */
         /* SHORTER than a full cover.  At ART_H the caption under it landed
          * at y=226 and y=244, and the 16 px "save present" line ran through
          * the footer divider (246) into the hint row (252) -- on every game
          * with no art.  152 puts the two lines at 202 and 220. */
         card_template(ART_X, ART_Y, ART_W, SHELF_CARD_H, &g_roms[rom]);
         bot = ART_Y + SHELF_CARD_H;
      }
      meta_line(&g_roms[rom], buf, sizeof(buf));
      vid_text(ART_X, bot + 10, buf, mix565(C_DIM, C_BG_TOP, g_fx_fade));
      vid_text(ART_X, bot + 28,
               rom_has_sav(rom) ? "save present" : "no save",
               mix565(rom_has_sav(rom) ? C_VALUE : C_DIM, C_BG_TOP,
                      g_fx_fade));
   }

   vid_rect(0, 246, 480, 1, C_CARD, 255);
   browser_footer(n, g_roms[view_rom(cur)].is_fav);
}

/* ---- SHELL B: Marquee -------------------------------------------------- */
#define MQ_ROW_H  24
#define MQ_MID    176            /* the selected row; everything else moves */

static int browser_rom_state_path(const char *rom_dir, int rom_idx,
                                  unsigned slot, char *out, size_t out_sz)
{
   char rom_path[PSP_FILE_PATH_CAP];
   if (psp_rom_path_join(rom_path, sizeof(rom_path), rom_dir,
                         g_roms[rom_idx].name) != 0)
      return -1;
   /* The browsing console is the console the game will boot as. */
   if (psp_rom_path_suffix(out, out_sz, rom_path,
                           psp_state_slot1_suffix(
                              (fe_console_t)g_pcfg.console)) != 0)
      return -1;
   return psp_state_path_for_slot(out, out_sz, out, slot);
}

static int browser_rom_has_state(const char *rom_dir, int rom_idx)
{
   SceIoStat st;
   char path[PSP_FILE_PATH_CAP];
   int slot, any = 0;
   for (slot = 1; slot <= PSP_STATE_SLOT_COUNT; slot++)
   {
      g_browser_state_exists[slot - 1] =
         browser_rom_state_path(rom_dir, rom_idx, (unsigned)slot,
                                path, sizeof(path)) == 0 &&
         fe_host_state_recover(path) >= 0 &&
         sceIoGetstat(path, &st) >= 0;
      any |= g_browser_state_exists[slot - 1];
   }
   return any;
}

static void browser_preview_load_one(const char *rom_dir, int rom_idx)
{
   int i;
   char state_path[PSP_FILE_PATH_CAP], thumb_path[PSP_STATE_THUMB_PATH_CAP];
   unsigned char hdr[PSP_STATE_THUMB_HEADER_SIZE];
   SceIoStat state_stat;
   SceUID fd;
   uint16_t *dst;
   size_t bytes = (size_t)PSP_STATE_THUMB_WIDTH * PSP_STATE_THUMB_HEIGHT * 2;
   if (!g_browser_state_open || !g_browser_previews)
      return;
   if (g_browser_preview_rom != rom_idx)
   {
      memset(g_browser_preview_status, 0, sizeof(g_browser_preview_status));
      memset(g_browser_previews, 0, PSP_STATE_SLOT_COUNT *
             PSP_STATE_THUMB_WIDTH * PSP_STATE_THUMB_TEX_HEIGHT * 2);
      g_browser_preview_rom = rom_idx;
   }
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
      if (!g_browser_preview_status[i])
         break;
   if (i == PSP_STATE_SLOT_COUNT)
      return;
   g_browser_preview_status[i] = 2; /* missing/invalid unless fully read */
   if (browser_rom_state_path(rom_dir, rom_idx, (unsigned)i + 1,
                              state_path, sizeof(state_path)) != 0 ||
       sceIoGetstat(state_path, &state_stat) < 0 ||
       psp_state_thumb_path(thumb_path, sizeof(thumb_path), state_path) != 0)
      return;
   fd = sceIoOpen(thumb_path, PSP_O_RDONLY, 0);
   if (fd < 0)
      return;
   if (sceIoRead(fd, hdr, sizeof(hdr)) != sizeof(hdr) ||
       memcmp(hdr, PSP_STATE_THUMB_MAGIC, 4) != 0 ||
       hdr[4] != PSP_STATE_THUMB_WIDTH || hdr[5] != 0 ||
       hdr[6] != PSP_STATE_THUMB_HEIGHT || hdr[7] != 0 ||
       hdr[8] != PSP_STATE_THUMB_WIDTH || hdr[9] != 0)
   {
      sceIoClose(fd);
      return;
   }
   dst = g_browser_previews + (size_t)i * PSP_STATE_THUMB_WIDTH *
         PSP_STATE_THUMB_TEX_HEIGHT;
   if (sceIoRead(fd, dst, bytes) == (int)bytes)
      g_browser_preview_status[i] = 1;
   sceIoClose(fd);
}

static void browser_state_cache_free(void)
{
   if (g_browser_previews)
      free(g_browser_previews);
   g_browser_previews = NULL;
   g_browser_state_open = 0;
   g_shelf_del = 0;
   g_browser_preview_rom = -1;
   g_browser_panel_t = 0;
   memset(g_browser_preview_status, 0, sizeof(g_browser_preview_status));
}

static void browser_state_panel(const char *rom_dir, int cur, int n)
{
   char title[64];
   int i, x;
   if (g_browser_state_open && g_browser_panel_t < PANEL_FRAMES)
      g_browser_panel_t++;
   else if (!g_browser_state_open && g_browser_panel_t > 0)
      g_browser_panel_t--;
   if (!g_browser_panel_t)
      return;
   /* Ease-out on the way in; running the same curve backwards on the way
    * out makes the close accelerate away, which reads as dismissal. */
   x = 480 - (202 * ease_out(g_browser_panel_t * 256 / PANEL_FRAMES) >> 8);
   /* Thumbnails only once the panel has landed: one read per frame then is
    * invisible, one read per frame during the slide was the stutter. */
   if (g_browser_state_open && g_browser_panel_t == PANEL_FRAMES)
      browser_preview_load_one(rom_dir, cur);
   vid_rect(x, 0, 480 - x, 272, C_BG_TOP, 255);
   vid_rect(x, 0, 2, 272, C_ACCENT, 255);
   vid_text(x + 14, 12, "SAVE STATES", C_SEL);
   rom_display_name(&g_roms[cur], title, sizeof(title), 1);
   clip_title(title, 174);
   vid_text(x + 14, 34, title, C_DIM);
   for (i = 0; i < PSP_STATE_SLOT_COUNT; i++)
   {
      /* The card is y-3..y+28 (32 rows).  Its two lines, 14 apart, are
       * placed as one block with their cap heights centred in it
       * (vid_text_y_in over the card less one line pitch): capitals from
       * y to y+24, 3 rows of card above and 4 below.  They were at y+1 and
       * y+15, which put the second line's capitals on the card's bottom
       * edge and "empty"'s descenders 4 px past it. */
      int y = 62 + i * 35, exists = g_browser_state_exists[i];
      int ty = vid_text_y_in(y - 3, 32 - 14);
      vid_rect(x + 10, y - 3, 182, 32,
               i + 1 == g_browser_state_slot ? C_CARD : C_BG_TOP, 255);
      if (g_browser_previews && g_browser_preview_status[i] == 1)
         vid_image(x + 14, y - 1, 42, 28,
                   g_browser_previews + (size_t)i * PSP_STATE_THUMB_WIDTH *
                      PSP_STATE_THUMB_TEX_HEIGHT,
                   PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_TEX_HEIGHT,
                   PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_HEIGHT, 255);
      else
      {
         vid_rect(x + 14, y - 1, 42, 28, C_HDR_BOT, 255);
         vid_text(x + 15, vid_text_y_in(y - 1, 28), exists ? "old" : "empty",
                  C_DIM);
      }
      {
         char slot[16];
         snprintf(slot, sizeof(slot), "Slot %d", i + 1);
         vid_text(x + 66, ty, slot, C_SEL);
      }
      vid_text(x + 66, ty + 14, exists ? "saved" : "empty",
               exists ? C_VALUE : C_DIM);
   }
   vid_rect(x + 10, 238, 182, 1, C_CARD, 255);
   if (g_shelf_del)
   {
      /* The question is a plate, as in game: the panel's hint lines give
       * way to it, so the answer keys are where the eye already is. */
      char q[32];
      snprintf(q, sizeof(q), "Delete Slot %d?", g_shelf_del);
      vid_rect(x + 2, 239, 480 - x - 2, 33, C_ACCENT, 40);
      vid_text(x + 14, 241, q, C_SEL);
      vid_text(x + 14, 254, "X: delete   O: keep", C_SEL);
   }
   else if (g_shelf_note[0])
   {
      vid_rect(x + 2, 239, 480 - x - 2, 33, C_ACCENT, 40);
      vid_text(x + 14, 241, g_shelf_note, C_SEL);
      vid_text(x + 14, 254, "X: load   O: close", C_DIM);
   }
   else
   {
      vid_text(x + 14, 241, g_browser_state_exists[g_browser_state_slot - 1]
                  ? "UP/DN: slot   " VID_GLYPH_SQ ": delete" : "UP/DN: slot",
               C_DIM);
      vid_text(x + 14, 254, "X: load   O: close", C_DIM);
   }
   (void)n;
}

static void shell_marquee(const char *rom_dir, int cur, int n, int n_scan,
                          int idle)
{
   int sy, rom = view_rom(cur);
   (void)rom_dir;   /* the hero cache is keyed by index, not path */

   /* APPLY THE THEME.  g_thm was only ever assigned inside page(), which
    * the browser does not call -- so the gallery rendered with whatever
    * palette the last menu screen happened to leave behind.  Boot went
    * to the browser dark; opening Settings and coming back made it
    * light.  Set it where the drawing happens. */
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;

   scroll_track(cur, n);
   /* The hero is a SEPARATE, larger decode of the same file -- the shelf
    * thumbnail stretched to 480 wide is the blurry mess this replaces. */
   const uint16_t *hero = hero_get(rom, idle >= IDLE_HERO && scroll_settled());
   char buf[64], title[64];
   int i;

   vid_rect(0, 0, 480, 272, C_BG_TOP, 255);
   if (hero)
   {
      /* Full-bleed COVER: scale to the screen width, keep the aspect, and
       * let the overflow crop off the top and bottom.  Fitting instead
       * would letterbox the background, which reads as a bug. */
      int bh = (int)((long)480 * g_hero_h / (g_hero_w ? g_hero_w : 1));
      /* Fades in on arrival, so a hero that took a moment to decode eases
       * into place rather than replacing the flat background in one frame.
       * And out again as the console switch fans the list away. */
      vid_image(0, (272 - bh) / 2, 480, bh, hero,
                g_hero_tex, g_hero_tex, g_hero_w, g_hero_h,
                (art_alpha(rom, hero) * (256 - g_fx_fade)) >> 8);
      /* Scrim by PROVENANCE.  Art from hero/ was composed for this shell --
       * already exposed for white text, already quiet on the left -- so it
       * needs a light touch.  A cover the emulator cropped itself has had no
       * such treatment and still needs the heavy one.  Using 205 on both
       * turns the good art black. */
      vid_rect(0, 0, 480, 272, C_BG_TOP,
               g_hero_precomposed ? 85 : 205);

      /* Footer band, drawn HERE rather than baked into the art -- baking it
       * put a hard horizontal seam across every image.  ONE alpha ramp, not
       * a stack of rects: four 8 px steps were invisible on the dark theme
       * and four obvious white bands on the light one, because the eye reads
       * banding against a pale ground far more easily. */
      vid_gradient_a(0, 232, 480, 40, C_BG_TOP, 0, 190);
   }

   vid_logo(HDR_LOGO_X, HDR_LOGO_Y, C_ACCENT, 255);
   browser_header(n_scan);

   {
      /* Drawn scrolled and then masked, so the text slides UNDER the edge of
       * the panel instead of appearing to run off the screen. */
      char full[64];
      int off;
      rom_display_name(&g_roms[rom], full, sizeof(full), 1);
      off = marquee_offset(rom, full, 244);
      vid_clip(20, 68, 244, 30);
      vid_text_hd(20 - off, 74 - g_fx_dy, full,
                  mix565(C_SEL, C_BG_TOP, g_fx_fade));
      vid_clip_off();
   }

   meta_line(&g_roms[rom], buf, sizeof(buf));
   vid_text(20, 102 - g_fx_dy, buf, mix565(C_ITEM, C_BG_TOP, g_fx_fade));
   vid_text(20 + vid_text_w(buf) + 14, 102 - g_fx_dy,
            rom_has_sav(rom) ? "save present" : "no save",
            mix565(rom_has_sav(rom) ? C_VALUE : C_DIM, C_BG_TOP, g_fx_fade));

   /* FIVE rows, not three.  At 24 px pitch they span 128..224, which clears
    * the metadata line (ends ~117) and the footer ramp (starts 240).  The
    * selected row stays at 176 so nothing else on the screen moves. */
   sy = scroll_px(cur, MQ_ROW_H) + g_fx_dy;
   for (i = -3; i <= 3; i++)          /* one row of overscan each side */
   {
      int idx = view_rom(((cur + i) % n + n) % n);
      int y   = MQ_MID + i * MQ_ROW_H + sy;
      int d   = i < 0 ? -i : i;
      int dx, fade;
      uint16_t col;
      /* Short libraries do not wrap (the Shelf's rule): with fewer games
       * than the five rows, a repeat reads as a bug, not a carousel.  This
       * was `n < 3` from the three-row marquee and never followed the row
       * count up, which is how a four-game favourites view showed its
       * first game twice. */
      if (n < 5 && (cur + i < 0 || cur + i >= n))
         continue;
      if (y < MQ_MID - 2 * MQ_ROW_H - 4 || y > MQ_MID + 2 * MQ_ROW_H + 4)
         continue;
      row_fx(i, &dx, &fade);
      rom_display_name(&g_roms[idx], title, sizeof(title), 1);
      clip_title(title, 232);
      /* The far rows fade out, so five entries do not read as a wall of
       * text competing with the artwork behind them. */
      col = d == 0 ? C_SEL : (d == 1 ? C_ITEM : C_DIM);
      vid_text(32 + dx, y, title, mix565(col, C_BG_TOP, fade));
      /* The star column, past the title's 232 px budget.  Full console
       * colour on the cursor row, walked halfway to the background on the
       * others so it marks without shouting. */
      if (g_roms[idx].is_fav && !(g_pop.active && g_pop.view == cur + i))
         vid_text(270 + dx, y, VID_GLYPH_STAR,
                  mix565(d == 0 ? C_CON : mix565(C_CON, C_BG_TOP, 115),
                         C_BG_TOP, fade));
   }
   /* The selection edge shrinks away with the outgoing list and grows back
    * with the new one. */
   {
      int h = g_fx_out ? (21 * (256 - g_fx_out)) >> 8
            : g_fx_in >= 0 ? (21 * ease_out((g_fx_in * 256) / SW_ROWS_IN)) >> 8
            : 21;
      if (h > 0)
         vid_rect(20, vid_band_y(MQ_MID, 21) + (21 - h) / 2, 3, h, C_CON,
                  255);
   }
   browser_pop_draw(270, MQ_MID + sy, MQ_ROW_H, cur);

   browser_footer(n, g_roms[rom].is_fav);
}

/* ---- empty states ---------------------------------------------------------
 * Both wear the console's identity, so a bare GBC folder still looks like
 * GBC.  The cartridge is eight rects: GBA carts are wide, GB/GBC carts tall
 * with the notch, and the label is the palette stripe.  The empty
 * favourites view shows the outline star it is asking for. */
static void browser_cart(int cx, int cy, const ui_skin *s)
{
   uint16_t body = g_pcfg.theme == 1 ? RGB565(0xD6, 0xD6, 0xD3)
                                     : RGB565(0x2C, 0x2C, 0x2C);
   uint16_t lip  = g_pcfg.theme == 1 ? RGB565(0xC2, 0xC2, 0xBE)
                                     : RGB565(0x3A, 0x3A, 0x3A);
   int k, lx, ly, lw, lh;
   if (s->cart_wide)
   {
      vid_rect(cx - 24, cy - 15, 48, 30, body, 255);
      vid_rect(cx - 24, cy - 15, 48, 4, lip, 255);
      lx = cx - 16; ly = cy - 6; lw = 32; lh = 14;
   }
   else
   {
      vid_rect(cx - 17, cy - 21, 28, 42, body, 255);
      vid_rect(cx + 11, cy - 15, 6, 36, body, 255);
      vid_rect(cx - 17, cy - 21, 34, 3, lip, 255);
      lx = cx - 11; ly = cy - 12; lw = 22; lh = 20;
   }
   vid_rect(lx, ly, lw, lh, C_BG_TOP, 120);
   for (k = 0; k < s->pal_n; k++)
      vid_rect(lx + (k * lw) / s->pal_n, ly,
               ((k + 1) * lw) / s->pal_n - (k * lw) / s->pal_n, lh,
               s->pal[k], 230);
}

static void browser_empty(const char *rom_dir, int n_scan)
{
   const ui_skin *s = skin_of(g_pcfg.console);
   char msg[64];
   uint16_t sel = mix565(C_SEL, C_BG_TOP, g_fx_fade);
   uint16_t item = mix565(C_ITEM, C_BG_TOP, g_fx_fade);
   uint16_t dim = mix565(C_DIM, C_BG_TOP, g_fx_fade);

   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;
   vid_rect(0, 0, 480, 272, C_BG_TOP, 255);
   vid_logo(HDR_LOGO_X, HDR_LOGO_Y, C_ACCENT, 255);
   browser_header(n_scan);
   if (g_browser_favs)
   {
      vid_text_hd(240 - vid_text_hd_w(VID_GLYPH_STAR_O) / 2, 88,
                  VID_GLYPH_STAR_O, mix565(C_CON, C_BG_TOP, g_fx_fade));
      snprintf(msg, sizeof(msg), "No %s favourites yet", s->id);
      vid_text_hd((480 - vid_text_hd_w(msg)) / 2, 126, msg, sel);
      vid_text_center(160, "Press " VID_GLYPH_SQ " on a game to star it", item);
      vid_text_center(182, "SELECT: all games", dim);
   }
   else
   {
      browser_cart(240, 104, s);
      snprintf(msg, sizeof(msg), "No .%s ROMs found", s->ext);
      vid_text_hd((480 - vid_text_hd_w(msg)) / 2, 130, msg, sel);
      /* The folder on one line when it fits, on its own line when not:
       * a memory stick path can be long. */
      snprintf(msg, sizeof(msg), "Copy them to %s", rom_dir);
      if (vid_text_w(msg) <= 440)
         vid_text_center(164, msg, item);
      else
      {
         vid_text_center(160, "Copy them to:", item);
         vid_text_center(180, rom_dir, item);
      }
      vid_text_center(vid_text_w(msg) <= 440 ? 186 : 204,
                      VID_GLYPH_TRI " change console", dim);
   }
   browser_footer(0, 0);
}

extern volatile int g_running;               /* main_psp exit flag */

/* START in the browser opens the SAME settings screen the in-game menu
 * uses -- previously the only way in was through a running game, so a
 * fresh install could not be configured at all.  Runs the screen
 * directly rather than through ui_frame(): the menu state machine is
 * for the in-game overlay, and ui_open() there would offer Resume and
 * Save state against a core that has not started.
 *
 * Returns 1 if a boot-time setting changed and the caller must relaunch
 * (Media Engine mode is latched before the browser runs), else 0. */
static int browser_demo(int frame, unsigned *edges);
static int g_bframe;             /* browser loop iterations, script clock */

/* Where the browser stood when START opened Settings. */
static struct { const char *rom_dir; int cur, n, n_scan, idle; } g_bpage;

/* The backdrop of START settings (docs/UI-OVERLAY.md, "Settings from the
 * browser").  The same screen_settings() the game uses, but with no game
 * frame to sit on: the browser's own page showed through the scrim as
 * clutter.  Instead the highlighted game's art is baked ONCE, here, into
 * the ambient texture (32 KiB of VRAM, no heap), and ov_backdrop() draws it
 * every frame under the scrim -- the loading screen's "L2" look.
 *
 * The art is whatever the browser already holds or would hold anyway:
 *   - the hero, when it is this game's (the Marquee's idle decode);
 *   - Marquee: hero_get() decodes into the texture the Marquee owns (it
 *     would after IDLE_HERO frames regardless);
 *   - Shelf: the cover in the box-art cache the shelf is showing.  The
 *     Shelf never holds a hero, and allocating one here would be a new
 *     512 KiB allocation, so a game with hero/ art shows its cover's bake.
 * Nothing found (no art, no ROM highlighted, an empty console, or
 * `loading_art = 0`): the console's palette ramp.  Returns 1 when art was
 * baked. */
static int browse_backdrop_take(void)
{
   unsigned t0 = sceKernelGetSystemTimeLow();
   int rom = -1, from = 0;       /* 1 hero held, 2 hero decoded, 3 cover */
   FE_EVT_ONLY(t0);
   if (g_pcfg.loading_art && g_bpage.rom_dir && g_bpage.n > 0)
      rom = view_rom(g_bpage.cur);
   if (rom >= 0)
   {
      if (g_hero_idx == rom && g_hero_state > 0)
         from = 1;
      else if (g_ui_shell && hero_get(rom, 1))
         from = 2;
      if (from)
         launch_art_take();
      else if (!g_ui_shell)
      {
         art_slot a;
         const uint16_t *tex = art_get(g_bpage.rom_dir, rom, 1, &a);
         /* The slot texture is ART_TEX_W x ART_TEX_H; aw x ah is what the
          * cover filled, top-left. */
         if (tex && a.aw > 0 && a.ah > 0)
         {
            from = 3;
            vid_ambient_bake_cover(tex, ART_TEX_W, a.aw,
                                   a.ah < ART_TEX_H ? a.ah : ART_TEX_H);
         }
      }
   }
   fe_evt("browse_backdrop rom=%s from=%d art=%d us=%u",
          rom >= 0 ? g_roms[rom].name : "-", from, vid_ambient_source() == 1,
          (unsigned)sceKernelGetSystemTimeLow() - t0);
   return vid_ambient_source() == 1;
}

#include "ui_share.inc"

static int browser_settings(void)
{
   int relaunch = 0;

   browse_backdrop_take();
   g_ov_browse = 1;

   /* Settings > Link mode edits the highlighted game (docs/GBA-LINK-UI.md):
    * its key, and its header code read once here, never per frame. */
   g_bset_have = 0;
   g_bset_key[0] = g_bset_code[0] = '\0';
   if (g_bpage.rom_dir && !g_lm_loaded)
      ui_linkmode_load(g_bpage.rom_dir);
   if (g_bpage.rom_dir && g_bpage.n > 0 && g_pcfg.console == FE_CONSOLE_GBA)
   {
      int rom = view_rom(g_bpage.cur);
      char path[PSP_FILE_PATH_CAP];
      SceUID fd;
      snprintf(g_bset_key, sizeof(g_bset_key), "%s", g_roms[rom].name);
      if (snprintf(path, sizeof(path), "%s/%s", g_bpage.rom_dir,
                   g_roms[rom].name) < (int)sizeof(path) &&
          (fd = sceIoOpen(path, PSP_O_RDONLY, 0)) >= 0)
      {
         if (sceIoLseek(fd, 0xAC, PSP_SEEK_SET) == 0xAC &&
             sceIoRead(fd, g_bset_code, 4) == 4)
            g_bset_code[4] = '\0';
         else
            g_bset_code[0] = '\0';
         sceIoClose(fd);
      }
      g_bset_have = 1;
   }

   screen_to(SCR_SETTINGS);
   g_cursor = SET_ROOM;
   g_prev_pad = 0xFFFFFFFFu;      /* swallow the opening START */
   /* Settings > Controls runs inside this loop too: it is a sub-page of
    * these settings, and it leaves back to them. */
   g_share_req = 0;
   while (g_running && !g_sh_quit &&
          (g_screen == SCR_SETTINGS || g_screen == SCR_CONTROLS ||
           g_screen == SCR_SCAN))
   {
      SceCtrlData pd;
      unsigned edges, pad;
      int join = 0;
      sceCtrlPeekBufferPositive(&pd, 1);
      pad = ui_demo_running() ? demo_pad() : pd.Buttons;
      edges = pad_edges(pad);
      /* The harness script keeps counting in here, so a shoot can walk the
       * Settings screen (no-op without a script). */
      browser_demo(++g_bframe, &edges);
      vid_overlay_begin(1);
      if (g_screen == SCR_CONTROLS)
         screen_controls(edges, pad);
      else if (g_screen == SCR_SCAN)
      {
         /* The room list, exactly as the in-game Wireless menu runs it.  A
          * join comes back as an action; backing out lands on Settings. */
         if (screen_scan(edges) == UI_ACT_NET_JOIN)
            join = 1;
         else if (g_screen != SCR_SCAN)
         {
            screen_to(SCR_SETTINGS);
            g_cursor = SET_JOIN;
         }
      }
      else if (screen_settings(edges) == UI_ACT_RELAUNCH)
      {
         relaunch = 1;
         g_screen = SCR_MENU;   /* screen_settings already consumed O */
      }
      vid_overlay_end();
      demo_dump_flush();
      sceDisplayWaitVblankStart();
      vid_swap();
      if (g_share_req == 1)
      {
         g_share_req = 0;
         share_host_run(g_bpage.rom_dir);
         screen_to(SCR_SETTINGS);
         g_cursor = SET_SHARE;
      }
      else if (g_share_req == 2)
      {
         g_share_req = 0;
         g_scan_count = -1;
         g_scan_room = -1;
         screen_to(SCR_SCAN);
      }
      if (join)
      {
         /* The room joined is a data share (the list lets nothing else be
          * joined from here): its menu opens by itself, and leaving it
          * lands back in the game list. */
         int num;
         if (room_parse(g_join_group, &num, NULL) == ROOM_SHARE)
            share_join_run(g_bpage.rom_dir, g_join_group);
         g_screen = SCR_MENU;
      }
   }
   /* Leave the ambient texture as the browser had it: nothing baked (the
    * browser runs before any launch).  A pick bakes its own at that time,
    * and a game with no art must not inherit this one's. */
   g_ov_browse = 0;
   vid_ambient_drop();
   if (g_settings_dirty)
   {
      pcfg_save();
      g_settings_dirty = 0;
   }
   /* Menu style needs no restart from here -- the browser is still the
    * thing drawing, so re-latching applies it on the next frame. */
   g_ui_shell = g_pcfg.ui_shell;
   g_prev_pad = 0xFFFFFFFFu;
   return relaunch;
}

#ifdef GPSP_CHEATS
/* RIGHT in the browser: the highlighted game's cheats, before it is launched
 * (docs/CHEATS.md).  The same page the in-game menu opens, over the same
 * backdrop START settings uses.  The list is loaded DETACHED: nothing reaches
 * the core here -- the choices are saved to the game's .on file, and the
 * launch reads that. */
static void browser_cheats(int rom)
{
   char title[100];
   int leave = 0;

   browse_backdrop_take();
   g_ov_browse = 1;
   ui_cheats_load(g_roms[rom].name, g_pcfg.console, 0);
   cht_page_reset();
   snprintf(title, sizeof(title), "%.99s", g_cht_stem);
   fe_evt("ui_screen name=cheats from=browser");
   g_prev_pad = 0xFFFFFFFFu;      /* swallow the opening RIGHT */
   while (g_running && !leave)
   {
      SceCtrlData pd;
      unsigned edges;
      sceCtrlPeekBufferPositive(&pd, 1);
      edges = pad_edges(pd.Buttons);
      browser_demo(++g_bframe, &edges);   /* the harness script keeps counting */
      vid_overlay_begin(1);
      leave = screen_cheats(edges, title);
      vid_overlay_end();
      demo_dump_flush();
      sceDisplayWaitVblankStart();
      vid_swap();
   }
   g_ov_browse = 0;
   vid_ambient_drop();
   ui_cheats_unload();
   g_prev_pad = 0xFFFFFFFFu;
}
#endif

/* ---- harness browser demo -------------------------------------------------
 * .gpsp-harness.ini ui_browser_demo=1 (with browser=1): a fixed script of
 * presses and GE dumps that walks the browser through every state the 3.0
 * UI has -- a tag, the favourites view, empty favourites, the console
 * switch mid-flare and at its caption, and a console with no ROMs -- so
 * tools/e2e can photograph them under PPSSPP.  Frames count browser loop
 * iterations; a dump at frame N shows the frame drawn at N-1.  The last
 * step exits the browser.  Reachable only through the harness ini, which a
 * release build points at a name that cannot exist (ADR-0067). */
static int g_ui_bdemo;
void ui_browser_demo_shots(void) { g_ui_bdemo = 1; }

typedef struct {
   unsigned short at;
   unsigned       btn;        /* edge to inject, or 0                     */
   const char    *dump;       /* log/<dump>.bmp to write, or NULL         */
} bdemo_step;

static const bdemo_step BDEMO[] = {
   /* The save-state shelf of the first game, when it has states (a no-op
    * otherwise): open, let the slide and the five thumbnails land, shoot,
    * close. */
   { 100, PSP_CTRL_LEFT,     NULL },
   { 135, 0,                 "ge_gallery_states" },
   { 140, PSP_CTRL_CIRCLE,   NULL },
   { 150, 0,                 "ge_gallery" },
   { 160, PSP_CTRL_SQUARE,   NULL },
   { 166, 0,                 "ge_gallery_star" },
   { 200, PSP_CTRL_SELECT,   NULL },
   { 240, 0,                 "ge_gallery_favs" },
   { 260, PSP_CTRL_SELECT,   NULL },
   { 300, PSP_CTRL_TRIANGLE, NULL },
   { 311, 0,                 "ge_gallery_flare" },
   { 322, 0,                 "ge_gallery_caption" },
   { 440, 0,                 "ge_gallery_gb" },
   { 460, PSP_CTRL_SELECT,   NULL },
   { 490, 0,                 "ge_gallery_favs_empty" },
   { 510, PSP_CTRL_SELECT,   NULL },
   { 540, PSP_CTRL_TRIANGLE, NULL },
   { 660, 0,                 "ge_gallery_empty" },
   { 680, PSP_CTRL_TRIANGLE, NULL },
   { 800, 0,                 NULL },          /* end: leave the browser */
};

static void browser_dump(const char *name)
{
   extern char g_dir_base[];
   char gp[176];
   snprintf(gp, sizeof(gp), "%s/log", g_dir_base);
   sceIoMkdir(gp, 0777);
   snprintf(gp, sizeof(gp), "%s/log/%s.bmp", g_dir_base, name);
   if (vid_dump_ge(gp) == 0)
      fe_evt("ge_dump file=%s.bmp ui=1", name);
}

/* ---- "Quit to game list": the hold and the crossfade ---------------------
 * (docs/INPROC-RETURN.md)  The player picks Quit to game list; the menu goes
 * and the picture it sat on stays, scrimmed exactly as the menu had it, with
 * one chip in the middle -- the loading screen's ring and "Returning to game
 * list...".  The game unloads under it (main_psp.c ticks the ring between
 * the teardown's steps, the ROM scan ticks it per game found), and the
 * browser's first UI_RETURN_FADE_FRAMES frames draw the same picture over
 * themselves at a falling alpha: a crossfade, with no black frame anywhere.
 *
 * When the unload is refused and the EBOOT relaunches instead, the hold is
 * what stays on screen until the firmware blanks it; the new process is
 * told (argv) and its browser fades in from black (ui_return_fade_in).
 *
 * No heap, no VRAM: the picture is the wake snapshot in the netdrv arena,
 * which outlives the game. */
#define RET_TICK_US  50000u
#define RET_CHIP_H   24
static int      g_ret_on;      /* 1 hold/crossfade, 2 fade in from black */
static int      g_ret_k;       /* crossfade frames drawn                  */
static unsigned g_ret_last;    /* last hold frame presented               */
static int      g_ret_dump;    /* harness `ret_dumps`: GE-dump the frames */
static int      g_ret_dump_k;  /* crossfade frame to dump after this draw */
static unsigned g_ret_n;       /* returns so far (dump names)             */
static unsigned g_ret_t0;      /* when the hold went up (EVT ms=)         */

/* The chip, centred: ring, then the line, on a plate in the accent colour
 * (the Controls page's bound chip, a row taller). */
static void ret_chip(int a, unsigned now)
{
   static const signed char ring[8][2] = {
      {0,-7}, {5,-5}, {7,0}, {5,5}, {0,7}, {-5,5}, {-7,0}, {-5,-5}
   };
   static const char msg[] = "Returning to game list...";
   int tw = vid_text_w(msg), w = 12 + 16 + 10 + tw + 14, i;
   int x = (VID_SCR_W - w) / 2, y = (VID_SCR_H - RET_CHIP_H) / 2;
   int cx = x + 12 + 8, cy = y + RET_CHIP_H / 2;
   vid_rect(x, y, w, RET_CHIP_H, C_BG_BOT, (200 * a) / 255);
   vid_rect(x, y, w, RET_CHIP_H, C_ACCENT, ((g_pcfg.theme == 1 ? 28 : 40) * a) / 255);
   vid_rect(x, y + RET_CHIP_H - 2, w, 2, skin_of(g_pcfg.console)->accent, a);
   for (i = 0; i < 8; i++)
      vid_rect(cx + ring[i][0] - 1, cy + ring[i][1] - 1, 3, 3, C_TITLE,
               ((50 + 25 * ((i + 8 - (now / 100000) % 8) % 8)) * a) / 255);
   vid_text_a(x + 12 + 16 + 10, vid_text_y_in(y, RET_CHIP_H), msg, C_SEL, a);
}

/* The hold picture at alpha `a` (255: exactly the menu's backdrop at its
 * resting scrim, minus the menu). */
static void ret_layer(int a, unsigned now)
{
   ov_theme();
   if (g_ret_on == 2)
   {
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, 0x0000, a);
      return;
   }
   if (g_ov_frame)
      vid_image_screen(g_ov_frame, 256, 256, g_ov_fw, g_ov_fh, a);
   vid_rect(0, 0, VID_SCR_W, VID_SCR_H, C_BG_BOT,
            ((g_ov_frame ? OV_SCRIM : 255) * a) / 255);
   vid_gradient_a(0, OV_RAMP_Y, VID_SCR_W, VID_SCR_H - OV_RAMP_Y, C_BG_BOT,
                  0, (165 * a) / 255);
   ret_chip(a, now);
}

static void ret_present(void)
{
   unsigned now = sceKernelGetSystemTimeLow();
   vid_overlay_begin(1);
   if (!g_ov_frame)
      vid_rect(0, 0, VID_SCR_W, VID_SCR_H, C_BG_BOT, 255);
   ret_layer(255, now);
   vid_overlay_end();
   if (g_ret_dump && !g_ret_last)
   {
      char nm[32];
      snprintf(nm, sizeof(nm), "ge_ret%u_hold", g_ret_n);
      browser_dump(nm);
   }
   sceDisplayWaitVblankStart();
   vid_swap();
   g_ret_last = now ? now : 1;
}

void ui_return_dumps(int on)
{
   g_ret_dump = on;
}

void ui_return_hold(void)
{
   g_ret_on = 1;
   g_ret_k = 0;
   g_ret_last = 0;
   g_ret_n++;
   g_ret_t0 = sceKernelGetSystemTimeLow();

   ret_present();
}

void ui_return_tick(void)
{
   if (g_ret_on == 1 &&
       sceKernelGetSystemTimeLow() - g_ret_last >= RET_TICK_US)
      ret_present();
}

static void ret_scan_tick(void)
{
   ui_return_tick();
}

void ui_return_fade_in(void)
{
   g_ret_on = 2;
   g_ret_k = 0;
}

/* The browser's frame is drawn; lay the hold over it at this frame's alpha.
 * Returns 1 while a fade is running (the caller swallows input: nothing acts
 * on a list the player cannot fully see yet). */
static int ret_fade_layer(void)
{
   int a;
   if (!g_ret_on)
      return 0;
   g_ret_k++;
   a = 255 * (UI_RETURN_FADE_FRAMES - g_ret_k) / UI_RETURN_FADE_FRAMES;
   g_ret_dump_k = g_ret_k;
   if (a <= 0)
   {
      fe_evt("ui_return fade_done mode=%d ms=%u", g_ret_on,
             (sceKernelGetSystemTimeLow() - g_ret_t0) / 1000u);
      g_ret_on = 0;
      return 0;
   }
   ret_layer(a, sceKernelGetSystemTimeLow());
   return 1;
}

/* Harness `ret_dumps = 1`: after the frame is drawn, before the swap. */
static void ret_dump_frame(void)
{
   char nm[32];
   int k = g_ret_dump_k;
   g_ret_dump_k = 0;
   if (!g_ret_dump || !k ||
       (k != 1 && k != 4 && k != 8 && k != 12 && k != 15 &&
        k != UI_RETURN_FADE_FRAMES))
      return;
   snprintf(nm, sizeof(nm), "ge_ret%u_fade%02d", g_ret_n, k);
   browser_dump(nm);
}

/* ---- harness browser script (ui_browser_script = file) ------------------
 * The fixed BDEMO table photographs the states; a README shoot also needs
 * per-frame sequences (the flare, the star pop, the shelf slide) and layouts
 * the table does not visit.  A text file, one step per line, frames counted
 * as above:
 *     <frame> press <UP|DOWN|LEFT|RIGHT|CROSS|CIRCLE|TRIANGLE|SQUARE|
 *                    SELECT|START|L|R>
 *     <frame> dump <name>              -> log/<name>.bmp
 *     <frame> seq <last_frame> <name>  -> log/<name>_<frame>.bmp, every frame
 *     <frame> exit
 * Harness only (reached through the ini a release build cannot read). */
#define BSCRIPT_MAX 192
typedef struct { unsigned short at, until; unsigned btn; char name[24]; unsigned char verb; } bscript_step;
enum { BS_PRESS = 1, BS_DUMP, BS_SEQ, BS_EXIT };
static bscript_step g_bscript[BSCRIPT_MAX];
static int g_bscript_n;

static unsigned bscript_button(const char *s)
{
   static const struct { const char *n; unsigned b; } tab[] = {
      { "UP", PSP_CTRL_UP }, { "DOWN", PSP_CTRL_DOWN }, { "LEFT", PSP_CTRL_LEFT },
      { "RIGHT", PSP_CTRL_RIGHT }, { "CROSS", PSP_CTRL_CROSS },
      { "CIRCLE", PSP_CTRL_CIRCLE }, { "TRIANGLE", PSP_CTRL_TRIANGLE },
      { "SQUARE", PSP_CTRL_SQUARE }, { "SELECT", PSP_CTRL_SELECT },
      { "START", PSP_CTRL_START }, { "L", PSP_CTRL_LTRIGGER },
      { "R", PSP_CTRL_RTRIGGER },
   };
   unsigned k;
   for (k = 0; k < sizeof(tab) / sizeof(tab[0]); k++)
      if (strcmp(tab[k].n, s) == 0)
         return tab[k].b;
   return 0;
}

void ui_browser_script_load(const char *rel)
{
   extern char g_dir_base[];
   char path[176], line[96];
   FILE *fp;
   snprintf(path, sizeof(path), "%s/%s", g_dir_base, rel);
   fp = fopen(path, "r");
   if (!fp)
   {
      fe_evt("ui_browser_script file=%s MISSING", rel);
      return;
   }
   while (g_bscript_n < BSCRIPT_MAX && fgets(line, sizeof(line), fp))
   {
      bscript_step *s = &g_bscript[g_bscript_n];
      char verb[16], arg[24], arg2[24];
      unsigned at;
      int n = sscanf(line, "%u %15s %23s %23s", &at, verb, arg, arg2);
      if (n < 2 || line[0] == '#')
         continue;
      memset(s, 0, sizeof(*s));
      s->at = (unsigned short)at;
      if (!strcmp(verb, "press") && n >= 3)
      {
         s->verb = BS_PRESS;
         s->btn = bscript_button(arg);
      }
      else if (!strcmp(verb, "dump") && n >= 3)
      {
         s->verb = BS_DUMP;
         snprintf(s->name, sizeof(s->name), "%s", arg);
      }
      else if (!strcmp(verb, "seq") && n >= 4)
      {
         s->verb = BS_SEQ;
         s->until = (unsigned short)strtoul(arg, NULL, 10);
         snprintf(s->name, sizeof(s->name), "%s", arg2);
      }
      else if (!strcmp(verb, "exit"))
         s->verb = BS_EXIT;
      else
         continue;
      g_bscript_n++;
   }
   fclose(fp);
   g_ui_bdemo = 1;
   fe_evt("ui_browser_script file=%s steps=%d", rel, g_bscript_n);
}

/* Returns 1 when the script wants the browser to exit. */
static int browser_demo(int frame, unsigned *edges)
{
   unsigned k;
   if (!g_ui_bdemo)
      return 0;
   if (g_bscript_n)
   {
      int i;
      for (i = 0; i < g_bscript_n; i++)
      {
         bscript_step *s = &g_bscript[i];
         if (s->verb == BS_SEQ)
         {
            if (frame >= s->at && frame <= s->until)
            {
               char nm[40];
               snprintf(nm, sizeof(nm), "%s_%04d", s->name, frame);
               browser_dump(nm);
            }
            continue;
         }
         if (s->at != frame)
            continue;
         if (s->verb == BS_PRESS)
            *edges |= s->btn;
         else if (s->verb == BS_DUMP)
            browser_dump(s->name);
         else if (s->verb == BS_EXIT)
            return 1;
      }
      return 0;
   }
   for (k = 0; k < sizeof(BDEMO) / sizeof(BDEMO[0]); k++)
      if (BDEMO[k].at == frame)
      {
         if (BDEMO[k].dump)
            browser_dump(BDEMO[k].dump);
         else if (!BDEMO[k].btn)
            return 1;
         *edges |= BDEMO[k].btn;
      }
   return 0;
}

/* ---- the favourites list swap ---------------------------------------------
 * SELECT, and an untag inside the favourites view, replace the list with a
 * different one.  Ten frames: the rows sink 10 px and fade, the list is
 * swapped at frame 5, the new rows rise into place. */
#define SWAP_FRAMES 10
static struct { int active, f, rebuild; } g_swap;

static void swap_begin(int rebuild)
{
   g_swap.active = 1;
   g_swap.f = 0;
   g_swap.rebuild = rebuild;
}

/* Advance; returns 1 on the frame the list must change. */
static int swap_step(void)
{
   int e;
   if (!g_swap.active)
      return 0;
   g_swap.f++;
   if (g_swap.f < SWAP_FRAMES / 2)
   {
      e = (g_swap.f * 256) / (SWAP_FRAMES / 2);
      g_fx_fade = e;
      g_fx_dy = (10 * ease_in(e)) >> 8;
   }
   else
   {
      e = ease_out(((g_swap.f - SWAP_FRAMES / 2) * 256) / (SWAP_FRAMES / 2));
      g_fx_fade = 256 - e;
      g_fx_dy = (10 * (256 - e)) >> 8;
   }
   if (g_swap.f >= SWAP_FRAMES)
   {
      g_swap.active = 0;
      fx_reset();
   }
   return g_swap.f == SWAP_FRAMES / 2;
}

/* The console switch's rescan: the old TRIANGLE handler, run at the peak of
 * the flare.  Re-scan using the strict extension for the newly selected
 * console; start at the saved game when compatible.  The state shelf is
 * keyed by list index, so it is closed and its cache dropped before the
 * list changes under it. */
static void browser_switch_apply(const char *rom_dir, int *n_scan, int *n,
                                 int *cur, int *idle)
{
   int i;
   if (pcfg_remember_console(g_sw.to) != 0)
      fe_log("Could not remember console: %d", g_sw.to);
   *n_scan = rom_scan(rom_dir, (fe_console_t)g_pcfg.console);
   if (*n_scan < 0)
      *n_scan = 0;
   favs_mark(*n_scan);
   *n = view_count(*n_scan);
   *cur = 0;
   for (i = 0; i < *n; i++)
      if (g_pcfg.last_rom[0] &&
          strcmp(g_roms[view_rom(i)].name, g_pcfg.last_rom) == 0)
         *cur = i;
   art_free_all();
   browser_state_cache_free();
   g_scroll_cur = -1;
   g_art_fade_idx = -1;
   g_marq_idx = -1;
   *idle = 0;
   g_sw.swapped = 1;
   fe_evt("ui_browser_console console=%d roms=%d found=%d",
          g_pcfg.console, *n_scan, g_rom_found);
}

int ui_browser(const char *rom_dir, char *out, size_t out_sz,
               int *out_state_slot, fe_console_t *console_out)
{
   int n_scan = rom_scan(rom_dir, (fe_console_t)g_pcfg.console);
   int n, cur = 0, i, idle = 0;

   if (out_state_slot)
      *out_state_slot = 0;
   if (console_out)
      *console_out = (fe_console_t)g_pcfg.console;

   if (n_scan < 0)
   {
      g_ret_on = 0;
      for (i = 0; i < 180 && g_running; i++)
      {
         vid_overlay_begin(1);
         page("GBAdhoc", NULL);
         vid_text_center(110, "ROM path is too long", C_WARN);
         vid_text_center(140, "Shorten the folder or filename", C_ITEM);
         vid_overlay_end();
         sceDisplayWaitVblankStart();
         vid_swap();
      }
      return -1;
   }

   /* n == 0 is NOT an exit here: another console's ROMs may be one
    * TRIANGLE away, so the empty notice is drawn inside the browser loop. */
   fe_evt("ui_browser console=%d roms=%d found=%d pool=%u",
          g_pcfg.console, n_scan, g_rom_found, g_rom_pool_used);

   /* A LIST THAT IS TOO LONG SAYS SO.  The 2.0 bug was not really the size
    * of the cap, it was that hitting it looked identical to the games not
    * being on the stick -- so whatever the ceiling is, crossing it has to be
    * visible.  Shown for ~2.5 s, then the browser opens as normal. */
   if (g_rom_found > n_scan || g_rom_scan_path_errors)
   {
      char msg[80];
      g_ret_on = 0;   /* a full-screen notice: nothing to fade from */
      const char *detail = g_rom_scan_path_errors
         ? "Some paths were too long and were skipped."
         : (n_scan == BROWSER_MAX ? "This build lists 1024 at a time."
                                  : "The name storage limit was reached.");
      if (g_rom_found > n_scan)
         snprintf(msg, sizeof(msg), "Showing %d of %d games", n_scan,
                  g_rom_found);
      else
         snprintf(msg, sizeof(msg), "Some ROM folders were skipped");
      for (i = 0; i < 150 && g_running; i++)
      {
         vid_overlay_begin(1);
         page("GBAdhoc", NULL);
         vid_text_center(104, msg, C_WARN);
         vid_text_center(134, detail, C_ITEM);
         if (g_rom_scan_path_errors)
            vid_text_center(156, "Shorten folders or filenames to browse them.", C_ITEM);
         else
         {
            vid_text_center(156, "Please open an issue on GitHub -- I want", C_ITEM);
            vid_text_center(178, "to know how big real libraries get.", C_ITEM);
         }
         vid_overlay_end();
         sceDisplayWaitVblankStart();
         vid_swap();
      }
   }

   /* Favourites: read once here, stamped onto the scan; the view opens on
    * the full list. */
   favs_load(rom_dir);
   favs_mark(n_scan);
   g_browser_favs = 0;
   n = n_scan;

   /* preselect last played */
   for (i = 0; i < n; i++)
      if (g_pcfg.last_rom[0] && strcmp(g_roms[i].name, g_pcfg.last_rom) == 0)
         cur = i;

   memset(g_art, 0, sizeof(g_art));
   for (i = 0; i < ART_SLOTS; i++)
      g_art[i].rom_idx = -1;
   g_ui_shell = g_pcfg.ui_shell;
   g_scroll_cur = -1;                 /* no slide on the first frame */
   g_art_fade_idx = -1;
   g_sw.active = 0;
   g_swap.active = 0;
   g_pop.active = 0;
   fx_reset();

   g_prev_pad = 0xFFFFFFFFu;
   while (g_running)
   {
      SceCtrlData pd;
      unsigned edges;
      int rom;                        /* g_roms index of the cursor row */

      sceCtrlPeekBufferPositive(&pd, 1);
      edges = pad_edges(pd.Buttons);
      g_bframe++;

      /* L+R+SELECT in the browser: dump the gallery screen as displayed
       * pixels (plain file I/O — works in every build, including the
       * telemetry-stripped playable, which is the only build whose browser
       * ever shows: a harness ini suppresses it by design, ADR-0067). */
      if ((pd.Buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT))
              == (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT))
      {
         static int shot_armed = 1;
         if (shot_armed)
         {
            browser_dump("ge_gallery");
            shot_armed = 0;
         }
      }

      /* Asset shoot: after the box art has streamed in, dump the gallery as
       * displayed pixels, then auto-pick so the run continues unattended
       * into the in-game menu demo. */
      if (g_ui_shots)
      {
         static int settle;
         if (++settle == 150)   /* let the box art fully stream in first */
         {
            browser_dump("ge_gallery");
            edges |= PSP_CTRL_CROSS;
         }
      }
      /* The crossfade from the game (ui_return_*): input waits for it. */
      if (g_ret_on)
         edges = 0;
      if (browser_demo(g_bframe, &edges))
         break;
      if (g_ctl_bdemo == 1 && g_bframe == 90)
      {
         g_ctl_bdemo = 2;
         ctl_bdemo_start();
         edges |= PSP_CTRL_START;
      }
      else if (g_ctl_bdemo == 2)
      {
         /* browser_settings() ran inside the previous iteration and has
          * returned, so the settings are closed: stop the script (its last
          * gap never plays out) and boot the game. */
         g_ctl_bdemo = 0;
         g_demo_on = -1;
         fe_evt("ui_demo_done");
         edges |= PSP_CTRL_CROSS;
      }

      /* TRIANGLE cycles the active hardware family, as a flare (see
       * switch_begin).  A press while one is running finishes it -- doing
       * the rescan now if the peak has not passed -- and starts the next
       * from the console it landed on.  The state shelf is keyed by list
       * index, so no switch starts while it is open. */
      if ((edges & PSP_CTRL_TRIANGLE) && !g_browser_state_open)
      {
         int skipped = 0;
         if (g_sw.active)
         {
            if (!g_sw.swapped)
               browser_switch_apply(rom_dir, &n_scan, &n, &cur, &idle);
            g_sw.active = 0;
            fx_reset();
            skipped = 1;
         }
         switch_begin((g_pcfg.console + 1) % FE_CONSOLE_COUNT);
         g_pop.active = 0;
         g_swap.active = 0;
         fe_evt("ui_browser_switch from=%d to=%d skipped=%d",
                g_pcfg.console, g_sw.to, skipped);
         FE_EVT_ONLY(skipped);
      }
      if (g_sw.active)
      {
         if (switch_step())
            browser_switch_apply(rom_dir, &n_scan, &n, &cur, &idle);
         /* No decodes and no input mid-flare: a cover decode would stall
          * the animation, and the list is about to change under the
          * cursor.  START is the one thing still honoured, below. */
         idle = 0;
         edges &= PSP_CTRL_START;
      }
      if (g_swap.active)
      {
         if (swap_step())
         {
            if (g_swap.rebuild)
               favs_build_view(n_scan);
            else
            {
               g_browser_favs = !g_browser_favs;
               fe_evt("ui_browser_view favs=%d shown=%d", g_browser_favs,
                      view_count(n_scan));
            }
            /* Stay on the same game when it is in both lists; when the
             * row itself left (an untag), stay where the cursor was. */
            {
               int found = 0, old_cur = cur;
               rom = n > 0 ? view_rom(cur) : -1;
               n = view_count(n_scan);
               cur = 0;
               for (i = 0; i < n; i++)
                  if (view_rom(i) == rom)
                  {
                     cur = i;
                     found = 1;
                  }
               if (!found && g_swap.rebuild && n > 0)
                  cur = old_cur < n ? old_cur : n - 1;
            }
            g_scroll_cur = -1;
            g_marq_idx = -1;
            g_art_fade_idx = -1;
            idle = 0;
         }
         edges &= PSP_CTRL_START;
      }
      if (pop_step() && !g_pop.add && g_browser_favs)
         swap_begin(1);                /* the untagged row leaves the view */

      rom = n > 0 ? view_rom(cur) : -1;

      /* SELECT swaps the list for the favourites subset and back.  Not
       * while L+R are held: that is the screenshot chord above. */
      if ((edges & PSP_CTRL_SELECT) && !g_browser_state_open &&
          !(pd.Buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)))
         swap_begin(0);

      /* SQUARE tags or untags the highlighted game and rewrites the file
       * at once.  In the favourites view an untag plays the pop first and
       * removes the row after it. */
      if ((edges & PSP_CTRL_SQUARE) && n > 0 && !g_browser_state_open &&
          !g_pop.active && !g_swap.active)
      {
         rom_entry *r = &g_roms[rom];
         int rc;
         if (r->is_fav)
         {
            rc = fe_favs_remove(&g_favs, r->name);
            r->is_fav = 0;
            g_fav_count--;
            if (!g_browser_favs)
               favs_build_view(n_scan);
            pop_begin(cur, 0);
         }
         else if ((rc = fe_favs_add(&g_favs, r->name)) == 0)
         {
            r->is_fav = 1;
            g_fav_count++;
            favs_build_view(n_scan);
            pop_begin(cur, 1);
         }
         else
            osd_toast("Favourites list is full");
         if (rc == 0)
            rc = favs_save(rom_dir);
         fe_evt("ui_browser_fav op=%s rom=%s n=%u rc=%d",
                r->is_fav ? "add" : "remove", r->name, g_favs.n, rc);
         idle = 0;
      }

      /* LEFT opens the selected game's save-state shelf, for every
       * console. Shoulder buttons page the list. */
      if (g_browser_state_open)
      {
         char state_path[PSP_FILE_PATH_CAP];
         SceIoStat st;
         /* SQUARE deletes the highlighted slot after an X on the panel's
          * plate; O or any other key keeps it, and while the question is
          * up nothing else acts (an X can never load instead). */
         if (g_shelf_del)
         {
            if (edges & PSP_CTRL_CROSS)
            {
               int s = g_shelf_del, ok;
               g_shelf_del = 0;
               ok = browser_rom_state_path(rom_dir, rom, 1, state_path,
                                           sizeof(state_path)) == 0 &&
                    state_delete(state_path, (unsigned)s) == 0;
               snprintf(g_shelf_note, sizeof(g_shelf_note),
                        ok ? "Slot %d deleted" : "Slot %d not deleted", s);
               /* Refresh what the shelf shows: the slot list from the stick,
                * and the deleted slot's preview dropped (status 2 = none),
                * so not even the closing slide shows it.  An empty shelf has
                * nothing left to offer: it closes. */
               if (ok)
                  g_browser_preview_status[s - 1] = 2;
               if (!browser_rom_has_state(rom_dir, rom))
               {
                  g_browser_state_open = 0;
                  idle = 0;
               }
            }
            else if (edges)
               g_shelf_del = 0;
            edges = 0;
         }
         if (edges)
            g_shelf_note[0] = '\0';
         if ((edges & PSP_CTRL_SQUARE) &&
             g_browser_state_exists[g_browser_state_slot - 1])
         {
            g_shelf_del = g_browser_state_slot;
            edges = 0;
         }
         if (edges & PSP_CTRL_CIRCLE)
         {
            g_browser_state_open = 0;
            idle = 0;
         }
         else if (edges & PSP_CTRL_UP)
            g_browser_state_slot = g_browser_state_slot > 1
               ? g_browser_state_slot - 1 : PSP_STATE_SLOT_COUNT;
         else if (edges & PSP_CTRL_DOWN)
            g_browser_state_slot = g_browser_state_slot < PSP_STATE_SLOT_COUNT
               ? g_browser_state_slot + 1 : 1;
         else if ((edges & PSP_CTRL_RIGHT) && !(edges & PSP_CTRL_CROSS))
            g_browser_state_open = 0;
         else if (edges & PSP_CTRL_CROSS)
         {
            if (browser_rom_state_path(rom_dir, rom,
                                       (unsigned)g_browser_state_slot,
                                       state_path, sizeof(state_path)) == 0 &&
                sceIoGetstat(state_path, &st) >= 0 &&
                psp_rom_path_join(out, out_sz, rom_dir,
                                  g_roms[rom].name) == 0)
            {
               if (out_state_slot)
                  *out_state_slot = g_browser_state_slot;
               if (console_out)
                  *console_out = (fe_console_t)g_pcfg.console;
               launch_art_from_browser(rom);
               ui_loading_begin(out);
               if (pcfg_remember_rom(g_roms[rom].name) != 0)
                  fe_log("Could not remember last ROM: %s", g_roms[rom].name);
               fe_evt("ui_browser_pick rom=%s state_slot=%d",
                      g_roms[rom].name, g_browser_state_slot);
               art_free_all();
               browser_state_cache_free();
               return 0;
            }
            osd_toast("No saved state in this slot");
         }
      }
      else if (n <= 0)
         ; /* nothing to move over; the empty notice below handles O */
      else if ((edges & PSP_CTRL_LEFT) && !g_ui_shots)
      {
         if (browser_rom_has_state(rom_dir, rom))
         {
            if (!g_browser_previews)
               g_browser_previews = (uint16_t *)memalign(16,
                  PSP_STATE_SLOT_COUNT * PSP_STATE_THUMB_WIDTH *
                  PSP_STATE_THUMB_TEX_HEIGHT * sizeof(uint16_t));
            g_browser_state_open = 1;
            g_browser_state_slot = 1;
            g_shelf_del = 0;
            g_shelf_note[0] = '\0';
            g_browser_preview_rom = -1;
            memset(g_browser_preview_status, 0,
                   sizeof(g_browser_preview_status));
            idle = 0;
         }
      }
#ifdef GPSP_CHEATS
      /* RIGHT opens the selected game's cheats (LEFT is its save states). */
      else if ((edges & PSP_CTRL_RIGHT) && !g_ui_shots)
      {
         g_bpage.rom_dir = rom_dir;
         g_bpage.cur = cur;
         g_bpage.n = n;
         g_bpage.n_scan = n_scan;
         g_bpage.idle = idle;
         browser_cheats(rom);
         idle = 0;
         continue;
      }
#endif
      else if (edges & PSP_CTRL_UP)
         { cur = (cur + n - 1) % n; idle = 0; }
      else if (edges & PSP_CTRL_DOWN)
         { cur = (cur + 1) % n; idle = 0; }
      else if (edges & PSP_CTRL_LTRIGGER)
         { cur = (cur + n - SHELF_ROWS % n) % n; idle = 0; }
      else if (edges & PSP_CTRL_RTRIGGER)
         { cur = (cur + SHELF_ROWS) % n; idle = 0; }
      if (edges & PSP_CTRL_START)
      {
         g_bpage.rom_dir = rom_dir;
         g_bpage.cur = cur;
         g_bpage.n = n;
         g_bpage.n_scan = n_scan;
         g_bpage.idle = idle;
         if (browser_settings())
         {
            art_free_all();
            browser_state_cache_free();
            return 1;
         }
         if (g_sh_quit)
            break;                    /* the harness script ended in there */
         if (g_sh_rescan)
         {
            /* Share game data walked the other consoles' ROMs, or a fetched
             * save changed what a row says: list this console again. */
            g_sh_rescan = 0;
            art_free_all();
            browser_state_cache_free();
            n_scan = rom_scan(rom_dir, (fe_console_t)g_pcfg.console);
            if (n_scan < 0)
               n_scan = 0;
            favs_mark(n_scan);
            n = view_count(n_scan);
            cur = 0;
            for (i = 0; i < n; i++)
               if (g_pcfg.last_rom[0] &&
                   strcmp(g_roms[view_rom(i)].name, g_pcfg.last_rom) == 0)
                  cur = i;
            g_scroll_cur = -1;
            g_art_fade_idx = -1;
            g_marq_idx = -1;
            g_sw.active = 0;
            fx_reset();
         }
         idle = 0;
         continue;
      }
      rom = n > 0 ? view_rom(cur) : -1;
      if ((edges & PSP_CTRL_CROSS) && n > 0)
      {
         if (g_browser_state_open)
            continue;
         if (psp_rom_path_join(out, out_sz, rom_dir, g_roms[rom].name) != 0)
         {
            osd_toast("ROM path is too long");
            fe_evt("ui_browser_pick rejected=path_too_long rel=%s",
                   g_roms[rom].name);
            continue;
         }
         if (console_out)
            *console_out = (fe_console_t)g_pcfg.console;
         launch_art_from_browser(rom);
         ui_loading_begin(out);
         if (pcfg_remember_rom(g_roms[rom].name) != 0)
            fe_log("Could not remember last ROM: %s", g_roms[rom].name);
         fe_evt("ui_browser_pick rom=%s", g_roms[rom].name);
         art_free_all();
         browser_state_cache_free();
         return 0;
      }

      if (n == 0)
      {
         /* No ROMs for this console, or no favourites in it.  The flare
          * still plays over this screen, so a switch out of an empty
          * console looks like any other. */
         vid_overlay_begin(1);
         browser_empty(rom_dir, n_scan);
         switch_draw();
         ret_fade_layer();
         vid_overlay_end();
         ret_dump_frame();
         sceDisplayWaitVblankStart();
         vid_swap();
         if ((edges & PSP_CTRL_CIRCLE) && !g_browser_favs)
            break;
         continue;
      }

      /* Asset shoot: force the idle state so art loads unconditionally —
       * under PPSSPP, host input polling can produce phantom pad edges that
       * reset `idle` every few frames, silently starving the art loader
       * (the placeholder-cards mystery: no failure, no load, no evidence). */
      if (g_ui_shots)
         idle = 100;

      vid_overlay_begin(1);

      /* Warm the cache around the cursor -- at most one decode per frame, so
       * holding a direction still scrolls smoothly. This is what removes the
       * art popping in as you move. */
      if (idle >= IDLE_PREFETCH && scroll_settled())
         art_prefetch(rom_dir, cur, n);

      if (g_ui_shell)
         shell_marquee(rom_dir, cur, n, n_scan, idle);
      else
         shell_shelf(rom_dir, cur, n, n_scan, idle);
      browser_state_panel(rom_dir, rom, n);
      switch_draw();
      ret_fade_layer();

      vid_overlay_end();
      ret_dump_frame();
      sceDisplayWaitVblankStart();
      vid_swap();
      if (idle < 1000)
         idle++;
   }
   art_free_all();
   browser_state_cache_free();
   return -1;
}
