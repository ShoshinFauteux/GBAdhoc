/* ui_share.inc -- "Share game data": the screens (docs/SAVE-SHARE.md).
 *
 * Included by ui_psp.c (it draws with the browser's own statics: the ROM
 * list, the hero art, the console flare).  The protocol and every file-
 * safety rule are frontend-common/fe_share.c, which the host test
 * exercises; the radio and the stick are psp/share_psp.c.  This file walks
 * rows and draws.
 *
 * BROWSER ONLY.  Both ends run from the ROM browser's START settings, with
 * no game loaded: share_host_run() is Settings > Share game data, and
 * share_join_run() is what the ordinary room list does when the room it
 * joined turns out to be a data share.  Nothing here is reachable from the
 * in-game menu, and nothing here runs unless one of those two is chosen.
 *
 * MEMORY.  Everything is malloc'd on entry and freed on exit (the browser
 * has the whole heap: no ROM is loaded).  Bounded: the sharer holds its
 * entry table, name pool and manifest (at most FE_SHARE_MAX_ENTRIES x
 * ~250 B); the guest holds the table, two sets of five 64x64 thumbnails
 * and one 82 KiB preview buffer.  No BSS beyond a few words. */

#include "fe_share.h"
#include "share_psp.h"

/* Size over speed for everything in this file (the reason is beside the
 * same switch in psp/Makefile); ui_psp.c's own code keeps its -O2. */
#pragma GCC push_options
#pragma GCC optimize ("Os")

/* ---- the two undecided defaults, one line each --------------------------- */
/* The per-game list follows the browser's Menu style: Marquee = the hero
 * art full-bleed behind it, Shelf = a flat page.  0 = always the hero. */
#define SHARE_LIST_FOLLOWS_SHELL 1
/* (Build mismatch: states greyed, saves live -- that one is
 * FE_SHARE_STATES_NEED_SAME_BUILD in fe_share.h.) */

/* Datagrams of file data the sharer sends per frame.  Four 1 KiB datagrams
 * a frame is ~245 KB/s, about what the bulk lane measured on real radios
 * (docs/GB-LINK.md); more only fills the other PSP's ring.  Harness
 * `share_burst`. */
static unsigned g_share_burst = 4;
void ui_share_burst(unsigned n) { g_share_burst = n ? n : 4; }

/* Secondary text.  These pages sit on a hero picture under a scrim, and on
 * the light theme the browser's C_DIM (#8C8C8C) washes out against it:
 * there the darker dim accent (#6B6B6B) is used instead. */
#define SH_DIM (g_pcfg.theme == 1 ? C_ACCENT_DK : C_DIM)

static int g_sh_quit;            /* the harness script said exit */
static int g_sh_rescan;          /* the browser's list must be rescanned */

/* ---- frame plumbing -------------------------------------------------------- */

static unsigned sh_begin(void)
{
   SceCtrlData pd;
   unsigned edges;
   sceCtrlPeekBufferPositive(&pd, 1);
   edges = pad_edges(pd.Buttons);
   if (browser_demo(++g_bframe, &edges))
      g_sh_quit = 1;
   vid_overlay_begin(1);
   g_thm = (g_pcfg.theme == 1) ? &THM_LIGHT : &THM_DARK;
   return edges;
}

static void sh_end(void)
{
   vid_overlay_end();
   sceDisplayWaitVblankStart();
   vid_swap();
}

/* ---- paths: a ROM's data files, the one place they are derived ----------- */

/* rom_dir/rel is the ROM; out = its save, or slot's state / thumb / shot.
 * The same rules as main_psp.c's save_path / state_path.  Share's slots are
 * the FIRST FE_SHARE_SLOTS (5) of this PSP's PSP_STATE_SLOT_COUNT (10): the
 * manifest is a wire format, so slots 6-10 are neither offered nor written,
 * and a Share slot must always be one this PSP has. */
typedef char share_slots_fit[FE_SHARE_SLOTS <= PSP_STATE_SLOT_COUNT ? 1 : -1];
static int share_path(char *out, size_t cap, const char *rom_dir,
                      const char *rel, int con, int kind, int slot)
{
   char rom[PSP_FILE_PATH_CAP], st[PSP_FILE_PATH_CAP];
   int n;
   out[0] = '\0';
   if (psp_rom_path_join(rom, sizeof(rom), rom_dir, rel) != 0)
      return -1;
   if (kind == FE_SHARE_OBJ_SAVE)
   {
      if (con == FE_CONSOLE_GBA)
         return psp_rom_path_suffix(out, cap, rom, ".sav");
      n = snprintf(out, cap, "%s.sav", rom);
      return (n > 0 && (size_t)n < cap) ? 0 : -1;
   }
   if (psp_rom_path_suffix(st, sizeof(st), rom,
                           psp_state_slot1_suffix((fe_console_t)con)) != 0 ||
       psp_state_path_for_slot(st, sizeof(st), st, (unsigned)slot) != 0)
      return -1;
   n = snprintf(out, cap, "%s%s", st,
                kind == FE_SHARE_OBJ_THUMB ? ".thumb" :
                kind == FE_SHARE_OBJ_SHOT ? ".shot" : "");
   return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int sh_rom_read(void *user, uint32_t off, void *buf, uint32_t len)
{
   SceUID fd = *(SceUID *)user;
   if (sceIoLseek32(fd, (int)off, PSP_SEEK_SET) != (int)off)
      return -1;
   return sceIoRead(fd, buf, len);
}

/* g_roms[idx]'s identity (one open, 1 + FE_SHARE_ID_SAMPLES short reads). */
static int sh_rom_id(const char *rom_dir, int idx, int con,
                     uint8_t id[FE_SHARE_ID_LEN])
{
   char path[PSP_FILE_PATH_CAP];
   SceUID fd;
   int rc;
   if (psp_rom_path_join(path, sizeof(path), rom_dir, g_roms[idx].name) != 0)
      return -1;
   fd = sceIoOpen(path, PSP_O_RDONLY, 0);
   if (fd < 0)
      return -1;
   rc = fe_share_game_id(con, g_roms[idx].size, sh_rom_read, &fd, id);
   sceIoClose(fd);
   return rc;
}

static uint32_t sh_stat_time(const SceIoStat *st)
{
   return fe_share_time_pack(st->sce_st_mtime.year, st->sce_st_mtime.month,
                             st->sce_st_mtime.day, st->sce_st_mtime.hour,
                             st->sce_st_mtime.minute);
}

static void sh_kb(char *out, size_t n, uint32_t bytes)
{
   snprintf(out, n, "%u KB", (unsigned)((bytes + 1023) / 1024));
}

/* ---- small drawings -------------------------------------------------------- */

static uint16_t sh_ok_col(void)
{
   return g_pcfg.theme == 1 ? RGB565(0x1E, 0x7A, 0x46)
                            : RGB565(0x6F, 0xCF, 0x97);
}

/* An 8x8 tick out of 2x2 squares: "got it". */
static void sh_tick(int x, int y, uint16_t col)
{
   static const signed char p[8][2] = { {0,4}, {1,5}, {2,6}, {3,5}, {4,4},
                                        {5,3}, {6,2}, {7,1} };
   int i;
   for (i = 0; i < 8; i++)
      vid_rect(x + p[i][0], y + p[i][1], 2, 2, col, 255);
}

/* THE SAVE ICON: Fable's eight 32x22 frames (the design's drawSaveIcon),
 * as rects rather than a texture -- a card drops into an open tray and
 * seats with a small overshoot (0-4), the tray's lip glints (5), a tick
 * lands (6-7).  `ink` is the row's text colour, `acc` the console's. */
static void sh_save_icon(int x, int y, int f, uint16_t ink, uint16_t acc)
{
   static const signed char card_y[8] = { -3, 5, 8, 10, 9, 9, 9, 9 };
   int cy = card_y[f & 7], top, h, i;
   /* tray: three 2 px strokes, open at the top */
   vid_rect(x + 6, y + 10, 2, 9, ink, 255);
   vid_rect(x + 24, y + 10, 2, 9, ink, 255);
   vid_rect(x + 6, y + 17, 20, 2, ink, 255);
   /* card (clipped at the frame's top edge while it is still above) */
   top = cy < 0 ? 0 : cy;
   h = cy + 7 - top;
   vid_rect(x + 10, y + top, 12, h, ink, f == 0 ? 215 : 255);
   if (cy + 3 >= 0)
      vid_rect(x + 13, y + cy + 3, 6, 1, C_BG_TOP, 150);   /* "data" line */
   if (f >= 1 && f <= 3)                                   /* motion ticks */
   {
      int a = 128 * (4 - f) / 3;
      vid_rect(x + 12, y + cy - 4, 1, 3, ink, a);
      vid_rect(x + 19, y + cy - 4, 1, 3, ink, a);
   }
   if (f == 5)                                             /* lip glint */
      vid_rect(x + 5, y + 8, 22, 2, acc, 255);
   if (f >= 6)                                             /* the tick */
   {
      static const signed char p[9][2] = { {21,4}, {22,5}, {23,6}, {24,7},
                                           {25,6}, {26,5}, {27,4}, {28,3},
                                           {29,2} };
      int n = f == 6 ? 7 : 9;
      for (i = 0; i < n; i++)
         vid_rect(x + p[i][0], y + p[i][1], 2, 2, acc, 255);
   }
}

/* The header row of every share screen: the browser's (wordmark, console
 * card on the 460 margin, the room where the shelf puts its games count),
 * plus the page title and a line under the wordmark.  `tri`: the TRIANGLE
 * hint hung on the card, where the browser hangs it. */
static void sh_header(const char *title, const char *sub, const char *group,
                      int tri, int card)
{
   char lbl[ROOM_LABEL_CAP], t[100];
   int x = HDR_CARD_R;
   vid_logo(HDR_LOGO_X, HDR_LOGO_Y, C_ACCENT, 255);
   if (title)
      vid_text_hd(HDR_LOGO_X + VID_LOGO_W + 14, HDR_LOGO_Y - 1, title, C_SEL);
   if (card)
   {
      x = hdr_card(skin_of(g_pcfg.console), g_fx_badge);
      if (tri)
      {
         x -= 4 + vid_text_w(VID_GLYPH_TRI);
         vid_text(x, HDR_TEXT_Y, VID_GLYPH_TRI, SH_DIM);
      }
      x -= 12;
   }
   room_label(group, lbl, sizeof(lbl));
   vid_text(x - vid_text_w(lbl), HDR_TEXT_Y, lbl, C_ACCENT_DK);
   if (sub && sub[0])
   {
      snprintf(t, sizeof(t), "%s", sub);
      clip_title(t, 440);
      vid_text(20, 34, t, SH_DIM);
   }
}

/* Hints along the bottom, laid out by measured width as the browser's. */
static void sh_hints(const char *const *hint, int n)
{
   int x = 20, i;
   vid_gradient_a(0, 232, 480, 40, C_BG_TOP, 0, 190);
   for (i = 0; i < n; i++)
   {
      vid_text(x, 252, hint[i], SH_DIM);
      x += vid_text_w(hint[i]) + 18;
   }
}

/* The page's ground: the game's hero art full-bleed under a scrim (the
 * Marquee's draw, a heavier scrim: these pages are lists), else flat. */
static void sh_backdrop(int rom, int want_hero)
{
   const uint16_t *hero = (rom >= 0 && want_hero) ? hero_get(rom, 1) : NULL;
   vid_rect(0, 0, 480, 272, C_BG_TOP, 255);
   if (hero)
   {
      int bh = (int)((long)480 * g_hero_h / (g_hero_w ? g_hero_w : 1));
      vid_image(0, (272 - bh) / 2, 480, bh, hero, g_hero_tex, g_hero_tex,
                g_hero_w, g_hero_h, 255 - ((255 * g_fx_fade) >> 8));
      vid_rect(0, 0, 480, 272, C_BG_TOP,
               g_pcfg.theme == 1 ? 205 : g_hero_precomposed ? 150 : 215);
   }
}

/* A centred plate with up to four lines and an optional bar (per mille,
 * -1 = none): the transfer, the question, the failure. */
static void sh_plate(const char *l1, const char *l2, const char *l3,
                     const char *l4, int frac)
{
   const int x = 50, w = 380;
   int lines = 1 + !!l2 + !!l3 + !!l4, h = 22 + lines * 18 +
               (frac >= 0 ? 14 : 0);
   int y = 128 - h / 2, ty;
   vid_rect(x + 3, y + 3, w, h, C_SHADOW, 110);
   vid_rect(x, y, w, h, C_CARD, 245);
   ov_outline(x, y, w, h, C_CON, 150);
   ty = y + 11;
   vid_text(x + 18, ty, l1, C_SEL);
   ty += 18;
   if (l2) { vid_text(x + 18, ty, l2, C_ITEM); ty += 18; }
   if (l3) { vid_text(x + 18, ty, l3, C_ITEM); ty += 18; }
   if (l4) { vid_text(x + 18, ty, l4, SH_DIM); ty += 18; }
   if (frac >= 0)
   {
      if (frac > 1000)
         frac = 1000;
      vid_rect(x + 18, ty + 4, w - 36, 5, C_ACCENT_DK, 110);
      if (frac > 0)
         vid_rect(x + 18, ty + 4, (w - 36) * frac / 1000, 5, C_CON, 255);
   }
}

/* A full-screen wait drawn once before something that blocks. */
static void sh_wait_frame(const char *title, const char *group,
                          const char *l1, const char *l2)
{
   static const char *const hint[] = { "please wait" };
   (void)sh_begin();
   sh_backdrop(-1, 0);
   sh_header(title, NULL, group, 0, 0);
   vid_text_center(104, l1, C_SEL);
   if (l2)
      vid_text_center(128, l2, SH_DIM);
   sh_hints(hint, 1);
   sh_end();
}

/* A notice the player dismisses (X or O), or the script ends. */
static void sh_notice(const char *title, const char *group, const char *l1,
                      const char *l2)
{
   static const char *const hint[] = { VID_GLYPH_O " back" };
   int k = 0;
   g_prev_pad = 0xFFFFFFFFu;
   while (g_running && !g_sh_quit)
   {
      unsigned edges = sh_begin();
      sh_backdrop(-1, 0);
      sh_header(title, NULL, group, 0, 0);
      vid_text_center(104, l1, C_SEL);
      if (l2)
         vid_text_center(128, l2, SH_DIM);
      sh_hints(hint, 1);
      sh_end();
      if (++k > 20 && (edges & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS)))
         break;
   }
   g_prev_pad = 0xFFFFFFFFu;
}

static const char *sh_net_error(int rc)
{
   return rc == ADHOC_ERR_WLAN_OFF ? "The WLAN switch is off." :
          rc == ADHOC_ERR_NO_GROUP ? "The room was not found. Is it still "
                                     "sharing?" :
          rc == ADHOC_ERR_NP_START ? "Not enough memory to start the radio." :
                                     "The radio could not start.";
}

/* ======================================================================== */
/* THE SHARER                                                                */
/* ======================================================================== */

#define SHH_POOL_MAX (96u * 1024u)

typedef struct
{
   const char *rom_dir;
   char group[9];
   fe_share_entry *e;
   unsigned *name_off;           /* entry -> its ROM's name in pool */
   short *rom_idx;               /* during the scan: entry -> g_roms index */
   unsigned n, cap;
   char *pool;
   unsigned pool_used, pool_cap;
   unsigned per_con[FE_SHARE_CONSOLES];
   unsigned dropped;             /* entries that did not fit */
   uint8_t *manifest;
   unsigned msize;
   fe_share_server srv;
   SceUID fd;                    /* the one file open for the guest */
} share_host_t;

static void shh_free(share_host_t *h)
{
   if (!h)
      return;
   if (h->fd >= 0)
      sceIoClose(h->fd);
   free(h->e);
   free(h->name_off);
   free(h->rom_idx);
   free(h->pool);
   free(h->manifest);
   free(h);
}

/* The entry for g_roms[rom] on `con`, made on first use.  -1 = full. */
static int shh_entry(share_host_t *h, short *map, int rom, int con)
{
   size_t len;
   if (map[rom] >= 0)
      return map[rom];
   if (h->n >= FE_SHARE_MAX_ENTRIES)
   {
      h->dropped++;
      return -1;
   }
   if (h->n == h->cap)
   {
      unsigned cap = h->cap ? h->cap * 2 : 32;
      void *e = realloc(h->e, cap * sizeof(*h->e));
      void *o, *r;
      if (!e) { h->dropped++; return -1; }
      h->e = (fe_share_entry *)e;
      o = realloc(h->name_off, cap * sizeof(*h->name_off));
      if (!o) { h->dropped++; return -1; }
      h->name_off = (unsigned *)o;
      r = realloc(h->rom_idx, cap * sizeof(*h->rom_idx));
      if (!r) { h->dropped++; return -1; }
      h->rom_idx = (short *)r;
      h->cap = cap;
   }
   len = strlen(g_roms[rom].name) + 1;
   if (h->pool_used + len > h->pool_cap)
   {
      unsigned cap = h->pool_cap ? h->pool_cap * 2 : 4096;
      void *p;
      while (cap < h->pool_used + len)
         cap *= 2;
      if (cap > SHH_POOL_MAX || !(p = realloc(h->pool, cap)))
      {
         h->dropped++;
         return -1;
      }
      h->pool = (char *)p;
      h->pool_cap = cap;
   }
   memset(&h->e[h->n], 0, sizeof(h->e[0]));
   h->e[h->n].console = (uint8_t)con;
   rom_display_name(&g_roms[rom], h->e[h->n].name, sizeof(h->e[0].name), 1);
   memcpy(h->pool + h->pool_used, g_roms[rom].name, len);
   h->name_off[h->n] = h->pool_used;
   h->pool_used += (unsigned)len;
   h->rom_idx[h->n] = (short)rom;
   map[rom] = (short)h->n;
   return (int)h->n++;
}

/* Walk roms/ as the browser's scan does (same depth, same skipped
 * folders) and note every save, state and preview beside a ROM of `con`.
 * The directory entries carry size and date: no stat per file. */
static void shh_walk(share_host_t *h, short *map, const char *rel, int depth,
                     int con, int n_roms)
{
   char dir[PSP_FILE_PATH_CAP];
   SceUID d;
   SceIoDirent ent;
   if (rel[0])
   {
      if (psp_rom_path_join(dir, sizeof(dir), h->rom_dir, rel) != 0)
         return;
   }
   else
      snprintf(dir, sizeof(dir), "%s", h->rom_dir);
   d = sceIoDopen(dir);
   if (d < 0)
      return;
   memset(&ent, 0, sizeof(ent));
   while (sceIoDread(d, &ent) > 0)
   {
      int is_dir = (ent.d_stat.st_mode & 0x1000) != 0 ||
                   (ent.d_stat.st_attr & 0x0010) != 0;
      int fcon, kind, slot, i, e;
      size_t klen;
      char key[PSP_ROM_REL_PATH_CAP];
      if (is_dir)
      {
         if (depth > 1 && ent.d_name[0] != '.')
         {
            char sub[PSP_ROM_REL_PATH_CAP];
            int n = rel[0] ? snprintf(sub, sizeof(sub), "%s/%s", rel,
                                      ent.d_name)
                           : snprintf(sub, sizeof(sub), "%s", ent.d_name);
            if (n > 0 && (size_t)n < sizeof(sub))
               shh_walk(h, map, sub, depth - 1, con, n_roms);
         }
      }
      else if (fe_share_classify(ent.d_name, &fcon, &kind, &slot, &klen) &&
               fcon == con && ent.d_stat.st_size > 0 &&
               ent.d_stat.st_size <= (SceOff)FE_SHARE_MAX_OBJECT)
      {
         int n = rel[0] ? snprintf(key, sizeof(key), "%s/%.*s", rel,
                                   (int)klen, ent.d_name)
                        : snprintf(key, sizeof(key), "%.*s", (int)klen,
                                   ent.d_name);
         if (n > 0 && (size_t)n < sizeof(key))
         {
            for (i = 0; i < n_roms; i++)
               if (fe_share_key_matches_rom(key, (size_t)n, con,
                                            g_roms[i].name))
                  break;
            if (i < n_roms && (e = shh_entry(h, map, i, con)) >= 0)
            {
               fe_share_entry *x = &h->e[e];
               uint32_t sz = (uint32_t)ent.d_stat.st_size;
               uint32_t tm = sh_stat_time(&ent.d_stat);
               if (kind == FE_SHARE_OBJ_SAVE)
               {
                  x->save_size = sz;
                  x->save_time = tm;
               }
               else if (kind == FE_SHARE_OBJ_STATE)
               {
                  x->st[slot - 1].size = sz;
                  x->st[slot - 1].time = tm;
               }
               else
                  x->st[slot - 1].flags |= kind == FE_SHARE_OBJ_THUMB
                     ? FE_SHARE_ST_THUMB : FE_SHARE_ST_SHOT;
            }
         }
      }
      memset(&ent, 0, sizeof(ent));
   }
   sceIoDclose(d);
}

/* Everything this PSP can offer, all three consoles.  Leaves g_roms
 * holding the last console scanned: the caller flags a browser rescan. */
static int shh_scan(share_host_t *h)
{
   short *map = (short *)malloc(BROWSER_MAX * sizeof(short));
   int con;
   unsigned t0 = sceKernelGetSystemTimeLow();
   FE_EVT_ONLY(t0);
   if (!map)
      return -1;
   for (con = 0; con < FE_SHARE_CONSOLES; con++)
   {
      unsigned first = h->n, i, k, w;
      int n_roms = rom_scan(h->rom_dir, (fe_console_t)con);
      if (n_roms <= 0)
         continue;
      memset(map, 0xFF, BROWSER_MAX * sizeof(short));
      shh_walk(h, map, "", ROM_SCAN_DEPTH, con, n_roms);
      /* Keep the entries that hold something and whose ROM can be
       * identified; a preview without its state is nothing. */
      for (i = w = first; i < h->n; i++)
      {
         fe_share_entry *x = &h->e[i];
         int any = x->save_size != 0;
         for (k = 0; k < FE_SHARE_SLOTS; k++)
         {
            if (!x->st[k].size)
               x->st[k].flags = 0;
            any |= x->st[k].size != 0;
         }
         if (!any || sh_rom_id(h->rom_dir, h->rom_idx[i], con, x->id) != 0)
            continue;
         h->e[w] = *x;
         h->name_off[w] = h->name_off[i];
         h->rom_idx[w] = h->rom_idx[i];
         w++;
      }
      h->n = w;
      h->per_con[con] = w - first;
   }
   free(map);
   g_sh_rescan = 1;
   h->msize = (unsigned)fe_share_manifest_size(h->n);
   h->manifest = (uint8_t *)malloc(h->msize);
   if (!h->manifest ||
       fe_share_manifest_build(h->e, h->n, h->manifest, h->msize) != h->msize)
      return -1;
   fe_evt("share_host_scan entries=%u gba=%u gb=%u gbc=%u dropped=%u "
          "pool=%u ms=%u", h->n, h->per_con[0], h->per_con[1], h->per_con[2],
          h->dropped, h->pool_used,
          (unsigned)(sceKernelGetSystemTimeLow() - t0) / 1000);
   return 0;
}

/* The server's file access: an entry index, a kind and a slot become a
 * path through THIS table.  Opened read-only; nothing here can write. */
static int shh_open(void *user, fe_share_obj o, uint32_t *size)
{
   share_host_t *h = (share_host_t *)user;
   char path[PSP_FILE_PATH_CAP + 8];
   int end;
   if (h->fd >= 0)
   {
      sceIoClose(h->fd);
      h->fd = -1;
   }
   if (o.entry >= h->n || !fe_share_obj_valid(h->e, h->n, o) ||
       o.kind == FE_SHARE_OBJ_MANIFEST ||
       share_path(path, sizeof(path), h->rom_dir,
                  h->pool + h->name_off[o.entry], h->e[o.entry].console,
                  o.kind, o.slot) != 0)
      return -1;
   h->fd = sceIoOpen(path, PSP_O_RDONLY, 0);
   if (h->fd < 0)
      return -1;
   end = sceIoLseek32(h->fd, 0, PSP_SEEK_END);
   if (end < 0)
   {
      sceIoClose(h->fd);
      h->fd = -1;
      return -1;
   }
   *size = (uint32_t)end;
   fe_evt("share_host_open entry=%u kind=%u slot=%u size=%d file=%s",
          o.entry, o.kind, o.slot, end, path);
   return 0;
}

static int shh_read(void *user, uint32_t off, void *buf, uint32_t len)
{
   share_host_t *h = (share_host_t *)user;
   if (h->fd < 0 || sceIoLseek32(h->fd, (int)off, PSP_SEEK_SET) != (int)off)
      return -1;
   return sceIoRead(h->fd, buf, len);
}

static void shh_close(void *user)
{
   share_host_t *h = (share_host_t *)user;
   if (h->fd >= 0)
      sceIoClose(h->fd);
   h->fd = -1;
}

/* Settings > Share game data.  Returns when the player stops sharing. */
static void share_host_run(const char *rom_dir)
{
   static const char *const hint[] = { VID_GLYPH_O " stop sharing" };
   static const fe_share_server_io io_proto = { NULL, share_net_send,
                                                shh_open, shh_read,
                                                shh_close };
   share_host_t *h = (share_host_t *)calloc(1, sizeof(*h));
   fe_share_server_io io = io_proto;
   fe_share_ident me;
   uint8_t *dg = NULL;
   char lbl[ROOM_LABEL_CAP], l1[96], l2[96], l3[96];
   int rc, was = -1, nn = room_number(g_pcfg.group);
   unsigned frame = 0;

   if (!h)
      return;
   h->fd = -1;
   h->rom_dir = rom_dir;
   room_make(ROOM_SHARE, nn < 0 ? 7 : nn, NULL, h->group, sizeof(h->group));
   room_label(h->group, lbl, sizeof(lbl));

   sh_wait_frame("SHARE GAME DATA", h->group, "Reading your saves and states...",
                 NULL);
   art_free_all();               /* the scans below re-key the ROM list */
   browser_state_cache_free();
   rc = shh_scan(h);
   if (rc != 0 || h->n == 0)
   {
      sh_notice("SHARE GAME DATA", h->group,
                rc ? "Could not read the game folder."
                   : "There are no saves or states to share yet.",
                rc ? NULL : "Play a game first; its save will show up here.");
      shh_free(h);
      return;
   }
   dg = (uint8_t *)malloc(FE_SHARE_DGRAM_MAX + 8);
   snprintf(l1, sizeof(l1), "Starting %s...", lbl);
   sh_wait_frame("SHARE GAME DATA", h->group, l1, NULL);
   rc = dg ? share_net_up(1, h->group) : ADHOC_ERR_NP_START;
   if (rc != ADHOC_OK)
   {
      sh_notice("SHARE GAME DATA", h->group, "Sharing could not start.",
                sh_net_error(rc));
      free(dg);
      shh_free(h);
      return;
   }
   share_ident(&me);
   io.user = h;
   fe_share_server_init(&h->srv, &io, &me, h->e, h->n, h->manifest, h->msize,
                        (uint32_t)(sceKernelGetSystemTimeLow() | 1u));
   fe_evt("share_host_up group=%s build=%s entries=%u saves=%u states=%u "
          "manifest=%u", h->group, me.build, h->n, h->srv.n_saves,
          h->srv.n_states, h->msize);

   g_prev_pad = 0xFFFFFFFFu;
   while (g_running && !g_sh_quit)
   {
      unsigned edges = sh_begin(), now = share_now_ms();
      const fe_share_server *s = &h->srv;
      int n;
      while ((n = share_net_recv(dg, FE_SHARE_DGRAM_MAX + 8)) > 0)
         fe_share_server_rx(&h->srv, dg, (size_t)n, now);
      fe_share_server_step(&h->srv, now, g_share_burst);
      if (s->state != was)
      {
         was = s->state;
         fe_evt("share_host_guest connected=%d build=%s rx_bad=%u refused=%u",
                was, was ? s->peer_ident.build : "-", s->rx_bad,
                s->rx_refused);
      }

      sh_backdrop(-1, 0);
      sh_header("SHARE GAME DATA", NULL, h->group, 0, 0);
      snprintf(l2, sizeof(l2), "%u saves  -  %u states  -  GBA %u / GB %u / "
               "GBC %u", s->n_saves, s->n_states, h->per_con[0],
               h->per_con[1], h->per_con[2]);
      l3[0] = '\0';
      if (s->state == FE_SHARE_SRV_CONNECTED)
      {
         snprintf(l1, sizeof(l1), "A PSP joined %s and is browsing.", lbl);
         if (s->have_obj && (s->obj.kind == FE_SHARE_OBJ_SAVE ||
                             s->obj.kind == FE_SHARE_OBJ_STATE))
         {
            char kb[16], nm[FE_SHARE_NAME_CAP];
            uint32_t nch = (s->obj_size + FE_SHARE_CHUNK - 1) / FE_SHARE_CHUNK;
            int pm = nch ? (int)(s->hi_chunk * 1000u / nch) : 1000;
            sh_kb(kb, sizeof(kb), s->obj_size);
            snprintf(nm, sizeof(nm), "%s", h->e[s->obj.entry].name);
            clip_title(nm, 200);
            if (s->obj.kind == FE_SHARE_OBJ_SAVE)
               snprintf(l3, sizeof(l3), "%s %s  -  save file  -  %s",
                        pm >= 1000 ? "Sent" : "Sending", nm, kb);
            else
               snprintf(l3, sizeof(l3), "%s %s  -  Slot %u state  -  %s",
                        pm >= 1000 ? "Sent" : "Sending", nm, s->obj.slot, kb);
            vid_rect(60, 172, 360, 5, C_ACCENT_DK, 110);
            vid_rect(60, 172, 360 * pm / 1000, 5, C_CON, 255);
         }
      }
      else
         snprintf(l1, sizeof(l1), "Sharing on %s. Waiting for another PSP.",
                  lbl);
      /* the pulse: the link host's dot, breathing while it waits */
      {
         int a = s->state == FE_SHARE_SRV_CONNECTED ? 255
                 : 90 + 165 * (int)((frame % 60) < 30 ? frame % 60
                                                       : 60 - frame % 60) / 30;
         int tw = vid_text_w(l1);
         vid_rect((480 - tw) / 2 - 14, 110, 8, 8,
                  s->state == FE_SHARE_SRV_CONNECTED ? sh_ok_col() : C_CON, a);
         vid_text_center(104, l1, C_SEL);
      }
      vid_text_center(128, l2, SH_DIM);
      if (l3[0])
         vid_text_center(150, l3, C_ITEM);
      else if (s->state != FE_SHARE_SRV_CONNECTED)
      {
         snprintf(l3, sizeof(l3), "On the other PSP: join %s as usual.", lbl);
         vid_text_center(150, l3, SH_DIM);
      }
      if (h->dropped)
         vid_text_center(196, "Some games did not fit in the list.", SH_DIM);
      sh_hints(hint, 1);
      sh_end();
      frame++;

      if (edges & PSP_CTRL_CIRCLE)
         break;
      if (!share_net_connected())
      {
         fe_evt("share_host_radio_lost");
         fe_share_server_stop(&h->srv);
         share_net_down();
         sh_notice("SHARE GAME DATA", h->group, "The radio dropped the room.",
                   "Sharing has stopped.");
         break;
      }
   }
   fe_evt("share_host_stop rx_ok=%u rx_bad=%u refused=%u tx_data=%u "
          "objects=%u", h->srv.rx_ok, h->srv.rx_bad, h->srv.rx_refused,
          h->srv.tx_data, h->srv.objects_served);
   if (share_net_is_up())
   {
      sh_wait_frame("SHARE GAME DATA", h->group, "Closing the room...", NULL);
      fe_share_server_stop(&h->srv);
      sceKernelDelayThread(60000);      /* let the BYE leave the radio */
      share_net_down();
   }
   free(dg);
   shh_free(h);
   g_prev_pad = 0xFFFFFFFFu;
}

/* ======================================================================== */
/* THE GUEST                                                                 */
/* ======================================================================== */

#define SHJ_THUMB_TEXELS (PSP_STATE_THUMB_WIDTH * PSP_STATE_THUMB_TEX_HEIGHT)
#define SHJ_OBUF (PSP_STATE_THUMB_HEADER_SIZE + PSP_STATE_SHOT_MAX_H * \
                  PSP_STATE_SHOT_STRIDE * 2)
#define SHJ_PATH (PSP_FILE_PATH_CAP + 16)

enum { JP_LIST, JP_THEIR, JP_YOURS };
enum { JM_NONE, JM_ASK, JM_XFER, JM_LOST, JM_FAIL };
enum { MARK_SAVE = 1 };          /* bits 1..5: their slot n was taken */
enum { TH_NONE = 0, TH_BUSY, TH_HAVE, TH_FAIL };

typedef struct
{
   const char *rom_dir;
   char group[9], room[ROOM_LABEL_CAP];
   fe_share_client cl;
   fe_share_ident me;
   uint32_t nonce;
   uint8_t *dg;

   fe_share_entry *e;
   unsigned n;
   short *local;                 /* entry -> g_roms index now, or -1 */
   unsigned char *mark;          /* session only: gone when the room is left */
   unsigned short *view;         /* the rows: entries of this console we own */
   int view_n, hidden, others;   /* not ours; entries of other consoles */
   int n_roms;

   int page, modal;
   int cur, top, idle;           /* the list */
   int ent, rom;                 /* the open game */
   int row;                      /* THEIR: 0 = save row, 1..5 = slots */
   int dwell, icon_t;
   int states_ok;
   int yslot;                    /* YOURS: 1..5 */

   /* the open game, here */
   int y_save, y_have[FE_SHARE_SLOTS];
   uint32_t y_save_time, y_save_size, y_time[FE_SHARE_SLOTS];
   uint16_t *ythumb, *tthumb;
   unsigned char y_th[FE_SHARE_SLOTS], t_th[FE_SHARE_SLOTS];
   unsigned char t_shot_fail[FE_SHARE_SLOTS];
   int shot_slot, shot_w, shot_h;      /* their slot in the VRAM texture */

   /* fetches into memory: the manifest, thumbnails, the full preview */
   uint8_t *obuf;
   uint32_t obuf_cap;
   int bg_kind, bg_slot;

   /* the transfer */
   int xf_kind, xf_src, xf_dst, xf_stage;
   uint32_t xf_t0, xf_bytes, xf_pm;
   share_file file;
   char fin[3][SHJ_PATH];        /* state/save, thumb, shot */
   int have_tmp[3];
   char note[64];                /* the line under the panel */
   int note_ok;
   char fail1[80], fail2[80];
} share_join_t;

static void shj_tmp_path(char *out, size_t cap, const char *fin)
{
   snprintf(out, cap, "%s" SHARE_TMP_SUFFIX, fin);
}

static void shj_drop_tmps(share_join_t *j)
{
   char tmp[SHJ_PATH + 8];
   int i;
   share_file_close(&j->file);
   for (i = 0; i < 3; i++)
      if (j->have_tmp[i])
      {
         shj_tmp_path(tmp, sizeof(tmp), j->fin[i]);
         sceIoRemove(tmp);
         j->have_tmp[i] = 0;
      }
}

static void shj_free(share_join_t *j)
{
   if (!j)
      return;
   shj_drop_tmps(j);
   free(j->dg);
   free(j->e);
   free(j->local);
   free(j->mark);
   free(j->view);
   free(j->ythumb);
   free(j->tthumb);
   free(j->obuf);
   free(j);
}

/* fe_share_sink over obuf */
static int shj_mem_write(void *user, uint32_t off, const void *buf,
                         uint32_t len)
{
   share_join_t *j = (share_join_t *)user;
   if (off > j->obuf_cap || len > j->obuf_cap - off)
      return -1;
   memcpy(j->obuf + off, buf, len);
   return 0;
}

static int shj_mem_read(void *user, uint32_t off, void *buf, uint32_t len)
{
   share_join_t *j = (share_join_t *)user;
   if (off > j->obuf_cap || len > j->obuf_cap - off)
      return -1;
   memcpy(buf, j->obuf + off, len);
   return (int)len;
}

static int shj_fetch_mem(share_join_t *j, int entry, int kind, int slot)
{
   fe_share_sink sink;
   fe_share_obj o;
   sink.user = j;
   sink.write = shj_mem_write;
   sink.read = shj_mem_read;
   sink.cap = j->obuf_cap;
   o.entry = (uint16_t)entry;
   o.kind = (uint8_t)kind;
   o.slot = (uint8_t)slot;
   return fe_share_client_fetch(&j->cl, o, &sink, share_now_ms());
}

static void shj_pump(share_join_t *j)
{
   unsigned now = share_now_ms();
   int n;
   while ((n = share_net_recv(j->dg, FE_SHARE_DGRAM_MAX + 8)) > 0)
      fe_share_client_rx(&j->cl, j->dg, (size_t)n, now);
   fe_share_client_step(&j->cl, now);
}

static int shj_busy(const share_join_t *j)
{
   return j->cl.state == FE_SHARE_CL_STAT || j->cl.state == FE_SHARE_CL_DATA;
}

/* Which of the sharer's games this PSP has, for the console on screen.
 * A ROM is read only when its size equals a shared game's (the scan gave
 * every size for free), so a big library costs a handful of opens. */
static void shj_build_view(share_join_t *j)
{
   int con = g_pcfg.console, i, k;
   unsigned e;
   j->view_n = j->hidden = j->others = 0;
   for (e = 0; e < j->n; e++)
   {
      uint32_t rsz;
      j->local[e] = -1;
      if (j->e[e].console != con)
      {
         j->others++;
         continue;
      }
      rsz = (uint32_t)j->e[e].id[0] | (uint32_t)j->e[e].id[1] << 8 |
            (uint32_t)j->e[e].id[2] << 16 | (uint32_t)j->e[e].id[3] << 24;
      for (i = 0; i < j->n_roms && j->local[e] < 0; i++)
      {
         uint8_t id[FE_SHARE_ID_LEN];
         if (g_roms[i].size != rsz)
            continue;
         if (sh_rom_id(j->rom_dir, i, con, id) == 0 &&
             memcmp(id, j->e[e].id, FE_SHARE_ID_LEN) == 0)
            j->local[e] = (short)i;
      }
      if (j->local[e] < 0)
      {
         j->hidden++;
         continue;
      }
      /* rows in the browser's order */
      for (k = j->view_n; k > 0 && j->local[j->view[k - 1]] > j->local[e]; k--)
         j->view[k] = j->view[k - 1];
      j->view[k] = (unsigned short)e;
      j->view_n++;
   }
   j->cur = j->top = 0;
   j->idle = 0;
   fe_evt("share_join_list console=%d shown=%d hidden=%d other_consoles=%d "
          "roms=%d", con, j->view_n, j->hidden, j->others, j->n_roms);
}

/* The open game's own data on this PSP: its save, its five slots and
 * their thumbnails.  Six stats and at most five 5 KiB reads, once per
 * open and after each transfer -- never per frame. */
static void shj_scan_yours(share_join_t *j)
{
   char path[SHJ_PATH];
   SceIoStat st;
   const char *rel = g_roms[j->rom].name;
   int con = g_pcfg.console, i;
   j->y_save = 0;
   j->y_save_time = j->y_save_size = 0;
   /* an interrupted replace is mended before the player's files are read
    * (fe_share_recover; no transfer is in progress here) */
   {
      char scratch[SHJ_PATH + 8];
      if (share_path(path, sizeof(path), j->rom_dir, rel, con,
                     FE_SHARE_OBJ_SAVE, 0) == 0 &&
          fe_share_recover(&share_fs, path, SHARE_TMP_SUFFIX, scratch,
                           sizeof(scratch)))
         fe_evt("share_recovered save");
      for (i = 0; i < FE_SHARE_SLOTS; i++)
         if (share_path(path, sizeof(path), j->rom_dir, rel, con,
                        FE_SHARE_OBJ_STATE, i + 1) == 0 &&
             fe_share_recover(&share_fs, path, SHARE_TMP_SUFFIX, scratch,
                              sizeof(scratch)))
            fe_evt("share_recovered slot=%d", i + 1);
   }
   if (share_path(path, sizeof(path), j->rom_dir, rel, con,
                  FE_SHARE_OBJ_SAVE, 0) == 0 && sceIoGetstat(path, &st) >= 0)
   {
      j->y_save = 1;
      j->y_save_time = sh_stat_time(&st);
      j->y_save_size = (uint32_t)st.st_size;
   }
   for (i = 0; i < FE_SHARE_SLOTS; i++)
   {
      j->y_have[i] = 0;
      j->y_th[i] = TH_NONE;
      j->y_time[i] = 0;
      if (share_path(path, sizeof(path), j->rom_dir, rel, con,
                     FE_SHARE_OBJ_STATE, i + 1) != 0 ||
          sceIoGetstat(path, &st) < 0)
         continue;
      j->y_have[i] = 1;
      j->y_time[i] = sh_stat_time(&st);
      if (j->ythumb && state_thumb_read(path, j->ythumb + (size_t)i *
                                              SHJ_THUMB_TEXELS) == 0)
         j->y_th[i] = TH_HAVE;
   }
   if (j->ythumb)
      sceKernelDcacheWritebackRange(j->ythumb,
                                    FE_SHARE_SLOTS * SHJ_THUMB_TEXELS * 2);
}

static int shj_row_ok(const share_join_t *j, int row)
{
   const fe_share_entry *x = &j->e[j->ent];
   if (row == 0)
      return x->save_size != 0;
   return j->states_ok && x->st[row - 1].size != 0;
}

static void shj_open_game(share_join_t *j, int view_row)
{
   int r;
   j->ent = j->view[view_row];
   j->rom = j->local[j->ent];
   j->states_ok = fe_share_states_ok(&j->me, &j->cl.host, g_pcfg.console);
   memset(j->t_th, 0, sizeof(j->t_th));
   memset(j->t_shot_fail, 0, sizeof(j->t_shot_fail));
   j->shot_slot = -1;
   g_ov_shot_slot = -1;          /* the in-game page's texture cache */
   j->note[0] = '\0';
   shj_scan_yours(j);
   /* land on the first state there is; else the save */
   j->row = 0;
   for (r = 1; r <= FE_SHARE_SLOTS; r++)
      if (shj_row_ok(j, r))
      {
         j->row = r;
         break;
      }
   j->dwell = 0;
   j->icon_t = 0;
   j->page = JP_THEIR;
   fe_evt("share_join_open entry=%d rom=%s states_ok=%d save=%u row=%d",
          j->ent, g_roms[j->rom].name, j->states_ok, j->e[j->ent].save_size,
          j->row);
}

/* A finished memory fetch: a thumbnail into its texture, the full preview
 * into the VRAM one (validated exactly as the local readers validate). */
static void shj_bg_done(share_join_t *j, int ok)
{
   int s = j->bg_slot - 1, kind = j->bg_kind;
   j->bg_kind = 0;
   if (kind == FE_SHARE_OBJ_THUMB)
   {
      const uint8_t *h = j->obuf;
      size_t bytes = (size_t)PSP_STATE_THUMB_WIDTH * PSP_STATE_THUMB_HEIGHT * 2;
      if (ok && j->cl.size == PSP_STATE_THUMB_HEADER_SIZE + bytes &&
          memcmp(h, PSP_STATE_THUMB_MAGIC, 4) == 0 &&
          h[4] == PSP_STATE_THUMB_WIDTH && h[5] == 0 &&
          h[6] == PSP_STATE_THUMB_HEIGHT && h[7] == 0 &&
          h[8] == PSP_STATE_THUMB_WIDTH && h[9] == 0)
      {
         memcpy(j->tthumb + (size_t)s * SHJ_THUMB_TEXELS,
                j->obuf + PSP_STATE_THUMB_HEADER_SIZE, bytes);
         sceKernelDcacheWritebackRange(j->tthumb + (size_t)s *
                                       SHJ_THUMB_TEXELS, SHJ_THUMB_TEXELS * 2);
         j->t_th[s] = TH_HAVE;
      }
      else
         j->t_th[s] = TH_FAIL;
      fe_evt("share_join_thumb slot=%d ok=%d", s + 1, j->t_th[s] == TH_HAVE);
   }
   else if (kind == FE_SHARE_OBJ_SHOT)
   {
      unsigned w = 0, h = 0;
      if (ok && j->cl.size >= 16 &&
          psp_state_shot_check(j->obuf, &w, &h) == 0 &&
          j->cl.size == 16 + h * PSP_STATE_SHOT_STRIDE * 2 && vid_shot_tex())
      {
         vid_shot_rows(0, j->obuf + 16, (int)h);
         j->shot_slot = s + 1;
         j->shot_w = (int)w;
         j->shot_h = (int)h;
         g_ov_shot_slot = -1;
      }
      else
         j->t_shot_fail[s] = 1;
      fe_evt("share_join_shot slot=%d ok=%d w=%u h=%u", s + 1,
             j->shot_slot == s + 1, w, h);
   }
}

/* Idle on Their states: bring in what the page still lacks, one small
 * fetch at a time -- the dwelt-on slot's full picture first, then the
 * thumbnails top to bottom. */
static void shj_bg_step(share_join_t *j)
{
   const fe_share_entry *x = &j->e[j->ent];
   int s;
   if (j->bg_kind)
   {
      if (j->cl.state == FE_SHARE_CL_DONE)
         shj_bg_done(j, 1);
      else if (j->cl.state == FE_SHARE_CL_ERROR)
         shj_bg_done(j, 0);
      else if (j->bg_kind == FE_SHARE_OBJ_SHOT && j->bg_slot != j->row)
      {
         /* the cursor moved on: this picture is no longer wanted */
         fe_share_client_cancel(&j->cl);
         j->bg_kind = 0;
      }
      return;
   }
   if (shj_busy(j) || !fe_share_client_linked(&j->cl) || !j->states_ok)
      return;
   s = j->row - 1;
   if (j->row >= 1 && j->dwell >= OV_SHOT_DWELL && x->st[s].size &&
       (x->st[s].flags & FE_SHARE_ST_SHOT) && j->shot_slot != j->row &&
       !j->t_shot_fail[s] && vid_shot_tex() &&
       (j->t_th[s] != TH_NONE || !(x->st[s].flags & FE_SHARE_ST_THUMB)))
   {
      if (shj_fetch_mem(j, j->ent, FE_SHARE_OBJ_SHOT, j->row) == 0)
      {
         j->bg_kind = FE_SHARE_OBJ_SHOT;
         j->bg_slot = j->row;
      }
      return;
   }
   /* the cursor's thumbnail first, then the rest */
   if (!(j->row >= 1 && x->st[s].size && (x->st[s].flags & FE_SHARE_ST_THUMB)
         && j->t_th[s] == TH_NONE))
      for (s = 0; s < FE_SHARE_SLOTS; s++)
         if (x->st[s].size && (x->st[s].flags & FE_SHARE_ST_THUMB) &&
             j->t_th[s] == TH_NONE)
            break;
   if (s < FE_SHARE_SLOTS && j->tthumb &&
       shj_fetch_mem(j, j->ent, FE_SHARE_OBJ_THUMB, s + 1) == 0)
   {
      j->t_th[s] = TH_BUSY;
      j->bg_kind = FE_SHARE_OBJ_THUMB;
      j->bg_slot = s + 1;
   }
}

static void shj_bg_stop(share_join_t *j)
{
   if (j->bg_kind)
   {
      fe_share_client_cancel(&j->cl);
      if (j->bg_kind == FE_SHARE_OBJ_THUMB)
         j->t_th[j->bg_slot - 1] = TH_NONE;
      j->bg_kind = 0;
   }
}

/* ---- the transfer ---------------------------------------------------------- */

static int shj_xf_fetch(share_join_t *j)
{
   static const int kind_of[3] = { FE_SHARE_OBJ_STATE, FE_SHARE_OBJ_THUMB,
                                   FE_SHARE_OBJ_SHOT };
   char tmp[SHJ_PATH + 8];
   fe_share_sink sink;
   fe_share_obj o;
   shj_tmp_path(tmp, sizeof(tmp), j->fin[j->xf_stage]);
   if (share_file_create(&j->file, tmp) != 0)
      return -1;
   j->have_tmp[j->xf_stage] = 1;
   sink.user = &j->file;
   sink.write = share_file_write;
   sink.read = share_file_read;
   sink.cap = 0;
   o.entry = (uint16_t)j->ent;
   o.kind = (uint8_t)(j->xf_kind == FE_SHARE_OBJ_SAVE ? FE_SHARE_OBJ_SAVE
                                                      : kind_of[j->xf_stage]);
   o.slot = (uint8_t)(j->xf_kind == FE_SHARE_OBJ_SAVE ? 0 : j->xf_src);
   {
      /* The file must be the one the manifest listed (review finding 4):
       * the sharer's STAT answer alone is not believed.  A save is also
       * never larger than a cartridge's (128 KiB + a clock trailer); a
       * state's previews (stages 1, 2) have no size in the manifest. */
      const fe_share_entry *x = &j->e[j->ent];
      uint32_t want = 0;
      if (j->xf_kind == FE_SHARE_OBJ_SAVE)
      {
         want = x->save_size;
         sink.cap = 0x20000u + 64u;
      }
      else if (j->xf_stage == 0 && j->xf_src >= 1 &&
               j->xf_src <= FE_SHARE_SLOTS)
         want = x->st[j->xf_src - 1].size;
      if ((j->xf_kind == FE_SHARE_OBJ_SAVE || j->xf_stage == 0) &&
          (!want || (sink.cap && want > sink.cap)))
         return -1;                 /* nothing listed, or not a save's size */
      if (fe_share_client_fetch(&j->cl, o, &sink, share_now_ms()) != 0)
         return -1;
      if (want)
         fe_share_client_expect(&j->cl, want);
      /* ...and a state must BE one of this build's (size, header): the
       * two builds agreeing in HELLO says nothing about the file. */
      if (j->xf_kind != FE_SHARE_OBJ_SAVE && j->xf_stage == 0)
         fe_share_client_expect_state(&j->cl, g_pcfg.console,
                                      j->me.state_fmt[g_pcfg.console]);
      return 0;
   }
}

static void shj_xf_fail(share_join_t *j, int err)
{
   int pct = j->cl.nchunks ? (int)(j->cl.got * 100u / j->cl.nchunks) : 0;
   shj_drop_tmps(j);
   fe_evt("share_xfer_fail kind=%d src=%d dst=%d stage=%d err=%s pct=%d",
          j->xf_kind, j->xf_src, j->xf_dst, j->xf_stage,
          fe_share_err_name(err), pct);
   if (j->xf_kind == FE_SHARE_OBJ_SAVE)
      snprintf(j->fail2, sizeof(j->fail2), "Your save was not changed.");
   else
      snprintf(j->fail2, sizeof(j->fail2), "Your Slot %d was not changed.",
               j->xf_dst);
   if (err == FE_SHARE_E_LOST || err == FE_SHARE_E_CLOSED)
   {
      snprintf(j->fail1, sizeof(j->fail1), err == FE_SHARE_E_CLOSED
               ? "The other PSP stopped sharing at %d%%."
               : "The transfer stopped at %d%%.", pct);
      j->modal = JM_LOST;
      return;
   }
   snprintf(j->fail1, sizeof(j->fail1), "%s",
            err == FE_SHARE_E_WRITE ? "Could not write it here. Is the "
                                      "Memory Stick full?" :
            err == FE_SHARE_E_CHANGED ? "The file changed on the other PSP "
                                        "while it was sent." :
            err == FE_SHARE_E_GONE ? "The other PSP no longer has that file." :
            err == FE_SHARE_E_TOO_BIG ? "That file is too large to fetch." :
            err == FE_SHARE_E_FORMAT ? "That is not a state this version "
                                       "can load." :
                                        "The transfer did not complete.");
   j->modal = JM_FAIL;
}

/* Start fetching their save (kind SAVE) or their slot `src` into our slot
 * `dst`.  Files land beside their final names as *.gsp; nothing of ours is
 * touched until every one of them has arrived and verified. */
static void shj_xf_start(share_join_t *j, int kind, int src, int dst)
{
   const char *rel = g_roms[j->rom].name;
   int con = g_pcfg.console, rc;
   shj_bg_stop(j);
   shj_drop_tmps(j);
   j->xf_kind = kind;
   j->xf_src = src;
   j->xf_dst = dst;
   j->xf_stage = 0;
   j->xf_t0 = share_now_ms();
   j->xf_bytes = 0;
   j->xf_pm = 0;
   j->note[0] = '\0';
   if (kind == FE_SHARE_OBJ_SAVE)
      rc = share_path(j->fin[0], sizeof(j->fin[0]), j->rom_dir, rel, con,
                      FE_SHARE_OBJ_SAVE, 0);
   else
      rc = share_path(j->fin[0], sizeof(j->fin[0]), j->rom_dir, rel, con,
                      FE_SHARE_OBJ_STATE, dst) ||
           share_path(j->fin[1], sizeof(j->fin[1]), j->rom_dir, rel, con,
                      FE_SHARE_OBJ_THUMB, dst) ||
           share_path(j->fin[2], sizeof(j->fin[2]), j->rom_dir, rel, con,
                      FE_SHARE_OBJ_SHOT, dst);
   fe_evt("share_xfer_start kind=%d src=%d dst=%d file=%s", kind, src, dst,
          j->fin[0]);
   j->modal = JM_XFER;
   if (rc != 0 || shj_xf_fetch(j) != 0)
      shj_xf_fail(j, fe_share_client_linked(&j->cl) ? FE_SHARE_E_WRITE
                                                    : FE_SHARE_E_LOST);
}

/* Everything arrived and verified: swap it in. */
static void shj_xf_commit(share_join_t *j)
{
   char tmp[SHJ_PATH + 8], scratch[SHJ_PATH + 8];
   unsigned ms = share_now_ms() - j->xf_t0;
   int rc, i;
   FE_EVT_ONLY(ms);
   pwr_hold("share_commit");
   shj_tmp_path(tmp, sizeof(tmp), j->fin[0]);
   rc = fe_share_commit(&share_fs, j->fin[0], tmp,
                        j->xf_kind == FE_SHARE_OBJ_SAVE, scratch,
                        sizeof(scratch));
   j->have_tmp[0] = 0;
   if (rc == 0 && j->xf_kind != FE_SHARE_OBJ_SAVE)
      /* The previews follow the state: the incoming one's, or none -- the
       * slot's old pictures must not stay beside a different state. */
      for (i = 1; i < 3; i++)
      {
         if (j->have_tmp[i])
         {
            shj_tmp_path(tmp, sizeof(tmp), j->fin[i]);
            fe_share_commit(&share_fs, j->fin[i], tmp, 0, scratch,
                            sizeof(scratch));
            j->have_tmp[i] = 0;
         }
         else
            sceIoRemove(j->fin[i]);
      }
   pwr_release();
   if (rc != 0)
   {
      /* -2: the old file could not be put back and is beside its name as
       * .bak / .old: try once more now, and only say "not changed" if it
       * is back.  (A load of the game mends it too: save_recover_bak.) */
      int lost = 0;
      if (rc == -2 &&
          !fe_share_recover(&share_fs, j->fin[0], SHARE_TMP_SUFFIX, scratch,
                            sizeof(scratch)))
         lost = 1;
      shj_xf_fail(j, FE_SHARE_E_WRITE);
      if (lost)
         snprintf(j->fail2, sizeof(j->fail2),
                  j->xf_kind == FE_SHARE_OBJ_SAVE
                     ? "Your old save is kept as a .bak file."
                     : "Your old Slot %d is kept as a .old file.", j->xf_dst);
      return;
   }
   fe_evt("share_xfer_done kind=%d src=%d dst=%d bytes=%u ms=%u kbps=%u "
          "retries=%u file=%s", j->xf_kind, j->xf_src, j->xf_dst,
          j->xf_bytes, ms, ms ? j->xf_bytes / ms : 0, j->cl.retries,
          j->fin[0]);
   if (j->xf_kind == FE_SHARE_OBJ_SAVE)
   {
      j->mark[j->ent] |= MARK_SAVE;
      snprintf(j->note, sizeof(j->note), j->y_save
               ? "Save file replaced yours" : "Save file saved");
      g_roms[j->rom].has_sav = -1;
   }
   else
   {
      j->mark[j->ent] |= (unsigned char)(1u << j->xf_src);
      snprintf(j->note, sizeof(j->note), "Saved to your Slot %d", j->xf_dst);
   }
   j->note_ok = 1;
   j->modal = JM_NONE;
   j->page = JP_THEIR;
   j->icon_t = 0;
   shj_scan_yours(j);
   g_sh_rescan = 1;
}

static void shj_xf_step(share_join_t *j)
{
   if (j->cl.state == FE_SHARE_CL_ERROR)
   {
      shj_xf_fail(j, j->cl.err);
      return;
   }
   if (j->cl.nchunks && j->xf_stage == 0)
      j->xf_pm = j->cl.got * 1000u / j->cl.nchunks;
   if (j->cl.state != FE_SHARE_CL_DONE)
      return;
   share_file_close(&j->file);
   j->xf_bytes += j->cl.size;
   if (j->xf_kind != FE_SHARE_OBJ_SAVE)
   {
      const fe_share_entry *x = &j->e[j->ent];
      unsigned fl = x->st[j->xf_src - 1].flags;
      while (++j->xf_stage < 3)
         if (fl & (j->xf_stage == 1 ? FE_SHARE_ST_THUMB : FE_SHARE_ST_SHOT))
         {
            if (shj_xf_fetch(j) != 0)
               shj_xf_fail(j, FE_SHARE_E_WRITE);
            return;
         }
   }
   shj_xf_commit(j);
}

static void shj_xf_cancel(share_join_t *j)
{
   fe_share_client_cancel(&j->cl);
   shj_drop_tmps(j);
   fe_evt("share_xfer_cancel kind=%d src=%d dst=%d", j->xf_kind, j->xf_src,
          j->xf_dst);
   j->modal = JM_NONE;
}

/* ---- joining --------------------------------------------------------------- */

/* HELLO -> WELCOME -> the manifest.  Draws its own frames; O cancels.
 * 0 = j->e / j->n hold the sharer's list. */
static int shj_connect(share_join_t *j)
{
   static const char *const hint[] = { VID_GLYPH_O " cancel" };
   fe_share_client_io io = { NULL, share_net_send };
   int stage = 0, rc = -1;
   char l1[80];
   share_ident(&j->me);
   fe_share_client_init(&j->cl, &io, &j->me, ++j->nonce);
   fe_share_client_hello(&j->cl, share_now_ms());
   g_prev_pad = 0xFFFFFFFFu;
   while (g_running && !g_sh_quit)
   {
      unsigned edges = sh_begin();
      shj_pump(j);
      if (stage == 0 && j->cl.state == FE_SHARE_CL_READY)
      {
         fe_evt("share_join_welcome build=%s mine=%s entries=%u saves=%u "
                "states=%u manifest=%u", j->cl.host.build, j->me.build,
                j->cl.n_entries, j->cl.n_saves, j->cl.n_states,
                j->cl.manifest_size);
         free(j->obuf);
         j->obuf_cap = j->cl.manifest_size > SHJ_OBUF ? j->cl.manifest_size
                                                      : SHJ_OBUF;
         j->obuf = (uint8_t *)malloc(j->obuf_cap);
         if (!j->obuf || shj_fetch_mem(j, 0, FE_SHARE_OBJ_MANIFEST, 0) != 0)
         {
            j->cl.err = FE_SHARE_E_WRITE;
            j->cl.state = FE_SHARE_CL_ERROR;
         }
         stage = 1;
      }
      else if (stage == 1 && j->cl.state == FE_SHARE_CL_DONE)
      {
         unsigned n = j->cl.n_entries, cap = n ? n : 1;
         free(j->e); free(j->local); free(j->mark); free(j->view);
         j->e = (fe_share_entry *)calloc(cap, sizeof(*j->e));
         j->local = (short *)calloc(cap, sizeof(short));
         j->mark = (unsigned char *)calloc(cap, 1);
         j->view = (unsigned short *)calloc(cap, sizeof(unsigned short));
         if (j->e && j->local && j->mark && j->view &&
             fe_share_manifest_parse(j->obuf, j->cl.size, j->e, n) == (int)n)
         {
            j->n = n;
            rc = 0;
         }
         else
         {
            j->cl.err = FE_SHARE_E_BAD;
            j->cl.state = FE_SHARE_CL_ERROR;
         }
      }
      sh_backdrop(-1, 0);
      sh_header("GAME DATA", NULL, j->group, 0, 0);
      snprintf(l1, sizeof(l1), stage ? "Reading what %s shares..."
                                     : "Looking for the PSP sharing on %s...",
               j->room);
      vid_text_center(104, l1, C_SEL);
      if (stage && j->cl.nchunks)
      {
         vid_rect(120, 132, 240, 4, C_ACCENT_DK, 110);
         vid_rect(120, 132, (int)(240u * j->cl.got / j->cl.nchunks), 4, C_CON,
                  255);
      }
      sh_hints(hint, 1);
      sh_end();
      if (rc == 0 || j->cl.state == FE_SHARE_CL_ERROR)
         break;
      if (edges & PSP_CTRL_CIRCLE)
      {
         j->cl.err = 0;
         break;
      }
      if (!share_net_connected())
      {
         j->cl.err = FE_SHARE_E_LOST;
         break;
      }
   }
   g_prev_pad = 0xFFFFFFFFu;
   if (rc != 0)
      fe_evt("share_join_connect_fail err=%s their_proto=%d their_build=%s",
             fe_share_err_name(j->cl.err), j->cl.refused_proto,
             j->cl.host.build);
   return rc;
}

/* Why a join did not get as far as a list, in the player's words. */
static void shj_connect_notice(share_join_t *j)
{
   char l2[96];
   switch (j->cl.err)
   {
   case 0:
      return;                                        /* the player's O */
   case FE_SHARE_E_REFUSED:
      snprintf(l2, sizeof(l2), "Update GBAdhoc on both PSPs (theirs: %.24s).",
               j->cl.host.build[0] ? j->cl.host.build : "another version");
      sh_notice("GAME DATA", j->group,
                "The two PSPs run versions that cannot share.", l2);
      break;
   case FE_SHARE_E_BUSY:
      sh_notice("GAME DATA", j->group, "Another PSP is already in that room.",
                "Try again when it has left.");
      break;
   default:
      snprintf(l2, sizeof(l2), "Ask them to choose Share game data again.");
      sh_notice("GAME DATA", j->group,
                "The PSP sharing on that room did not answer.", l2);
      break;
   }
}

/* The console switch's rescan, at the flare's peak: the browser's own
 * (remembered console, strict extension), then the sharer's rows for it. */
static void shj_switch_apply(share_join_t *j)
{
   if (pcfg_remember_console(g_sw.to) != 0)
      fe_log("Could not remember console: %d", g_sw.to);
   j->n_roms = rom_scan(j->rom_dir, (fe_console_t)g_pcfg.console);
   if (j->n_roms < 0)
      j->n_roms = 0;
   art_free_all();
   browser_state_cache_free();
   g_scroll_cur = -1;
   g_art_fade_idx = -1;
   g_marq_idx = -1;
   g_sw.swapped = 1;
   g_sh_rescan = 1;
   shj_build_view(j);
}

/* ---- drawing --------------------------------------------------------------- */

#define SHL_ROWS   5
#define SHL_ROW_Y0 106
#define SHL_ROW_H  26

static void shj_holds(const fe_share_entry *x, int states_ok, char *out,
                      size_t n)
{
   int k, st = 0;
   for (k = 0; k < FE_SHARE_SLOTS; k++)
      st += x->st[k].size != 0;
   if (x->save_size && st)
      snprintf(out, n, "SAVE  -  %d state%s%s", st, st == 1 ? "" : "s",
               states_ok ? "" : " (not for this build)");
   else if (x->save_size)
      snprintf(out, n, "SAVE");
   else
      snprintf(out, n, "%d state%s%s", st, st == 1 ? "" : "s",
               states_ok ? "" : " (not for this build)");
}

static void shj_took(unsigned mark, char *out, size_t n)
{
   int k, st = 0;
   for (k = 1; k <= FE_SHARE_SLOTS; k++)
      st += (mark >> k) & 1;
   if ((mark & MARK_SAVE) && st)
      snprintf(out, n, "save, %d state%s", st, st == 1 ? "" : "s");
   else if (mark & MARK_SAVE)
      snprintf(out, n, "save");
   else if (st)
      snprintf(out, n, "%d state%s", st, st == 1 ? "" : "s");
   else
      out[0] = '\0';
}

static void shj_draw_list(share_join_t *j)
{
   static const char *const hint[] = { VID_GLYPH_X " their states", "L/R page",
                                       VID_GLYPH_TRI " console",
                                       VID_GLYPH_O " leave" };
   static const char *const hint_empty[] = { VID_GLYPH_TRI " console",
                                             VID_GLYPH_O " leave" };
   char sub[96], t[100], r[64];
   int i, hero_shell = SHARE_LIST_FOLLOWS_SHELL ? g_ui_shell : 1;
   int states_ok = fe_share_states_ok(&j->me, &j->cl.host, g_pcfg.console);

   if (states_ok)
      snprintf(sub, sizeof(sub), "Shared by %s", j->room);
   else
      snprintf(sub, sizeof(sub), "%s  -  their build %.20s, yours %.20s",
               j->room, j->cl.host.build, j->me.build);
   if (j->view_n == 0)
   {
      const ui_skin *s = skin_of(g_pcfg.console);
      sh_backdrop(-1, 0);
      sh_header("GAME DATA", sub, j->group, 1, 1);
      browser_cart(240, 100, s);
      snprintf(t, sizeof(t), j->hidden
               ? "Nothing here for your %s games" : "No %s saves or states on %s",
               s->full, j->room);
      vid_text_center(138, t, mix565(C_SEL, C_BG_TOP, g_fx_fade));
      if (j->others)
         vid_text_center(160, "They share some for the other consoles.",
                         mix565(C_ITEM, C_BG_TOP, g_fx_fade));
      vid_text_center(184, VID_GLYPH_TRI " change console",
                      mix565(SH_DIM, C_BG_TOP, g_fx_fade));
      sh_hints(hint_empty, 2);
      return;
   }
   {
      int e = j->view[j->cur], rom = j->local[e];
      const fe_share_entry *x = &j->e[e];
      sh_backdrop(rom, hero_shell && j->idle >= IDLE_HERO);
      sh_header("GAME DATA", sub, j->group, 1, 1);
      rom_display_name(&g_roms[rom], t, sizeof(t), 1);
      clip_title(t, 380);
      vid_text_hd(20, 54 - g_fx_dy, t, mix565(C_SEL, C_BG_TOP, g_fx_fade));
      shj_holds(x, states_ok, t, sizeof(t));
      if (j->mark[e])
      {
         shj_took(j->mark[e], r, sizeof(r));
         snprintf(t + strlen(t), sizeof(t) - strlen(t), "  -  you took the %s",
                  r);
      }
      else if (states_ok)
         snprintf(t + strlen(t), sizeof(t) - strlen(t), "  -  %s",
                  rom_has_sav(rom) ? "you have a save for this game"
                                   : "you have no save for this game");
      vid_text(20, 80 - g_fx_dy, t, mix565(C_ITEM, C_BG_TOP, g_fx_fade));
   }
   if (j->cur < j->top)
      j->top = j->cur;
   if (j->cur >= j->top + SHL_ROWS)
      j->top = j->cur - SHL_ROWS + 1;
   for (i = 0; i < SHL_ROWS && j->top + i < j->view_n; i++)
   {
      int v = j->top + i, e = j->view[v], sel = v == j->cur;
      int y = SHL_ROW_Y0 + i * SHL_ROW_H + g_fx_dy, dx, fade, rx;
      const fe_share_entry *x = &j->e[e];
      row_fx(v - j->cur < -3 ? -3 : v - j->cur > 3 ? 3 : v - j->cur, &dx,
             &fade);
      if (sel && !g_fx_out && g_fx_in < 0)
      {
         vid_rect(0, vid_band_y(y, 24), 480, 24, C_CON, 56);
         vid_rect(0, vid_band_y(y, 24), 4, 24, C_CON, 255);
      }
      rom_display_name(&g_roms[j->local[e]], t, sizeof(t), 1);
      clip_title(t, 250);
      vid_text(20 + dx, y, t, mix565(sel ? C_SEL : C_ITEM, C_BG_TOP, fade));
      shj_holds(x, 1, r, sizeof(r));
      rx = 462 - vid_text_w(r) + dx;
      vid_text(rx, y, r, mix565(sel ? C_VALUE : C_ACCENT_DK, C_BG_TOP, fade));
      if (j->mark[e] && fade < 128)
      {
         shj_took(j->mark[e], r, sizeof(r));
         rx -= 14 + vid_text_w(r);
         vid_text(rx, y, r, sh_ok_col());
         sh_tick(rx - 12, y + 4, sh_ok_col());
      }
   }
   if (j->view_n > SHL_ROWS)
      ov_scrollbar(474, SHL_ROW_Y0, SHL_ROWS * SHL_ROW_H, j->top * SHL_ROW_H,
                   SHL_ROWS * SHL_ROW_H, j->view_n * SHL_ROW_H);
   sh_hints(hint, 4);
}

/* Their states / Your slots share a geometry: the save-state page's.  Six
 * rows on the left (the save row, a hairline, five slots), the panel 1:1
 * on the right with the runner along its bottom edge. */
#define SHT_SAVE_Y  56
#define SHT_SLOT_Y(i) (90 + 31 * (i))
#define SHT_LIST_W  212

/* The panel's frame, with whatever is inside drawn by the caller first. */
static void shj_runner(const char *txt)
{
   char t[80];
   snprintf(t, sizeof(t), "%s", txt);
   clip_title(t, SS_PW - 14);
   vid_rect(SS_PX, SS_PY + SS_PH - 18, SS_PW, 18, 0x0000, 178);
   vid_text(SS_PX + 7, SS_PY + SS_PH - 17, t, 0xE71C);
}

/* The game's hero art in the preview frame: cover, centred. */
static void shj_panel_hero(share_join_t *j)
{
   const uint16_t *hero = hero_get(j->rom, 1);
   vid_rect(SS_PX + 3, SS_PY + 3, SS_PW, SS_PH, C_SHADOW, 90);
   vid_rect(SS_PX, SS_PY, SS_PW, SS_PH, C_CARD, 255);
   if (hero && g_hero_w > 0 && g_hero_h > 0)
   {
      int w = SS_PW, h = (int)((long)SS_PW * g_hero_h / g_hero_w);
      if (h < SS_PH)
      {
         h = SS_PH;
         w = (int)((long)SS_PH * g_hero_w / g_hero_h);
      }
      vid_clip(SS_PX, SS_PY, SS_PW, SS_PH);
      vid_image(SS_PX + (SS_PW - w) / 2, SS_PY + (SS_PH - h) / 2, w, h, hero,
                g_hero_tex, g_hero_tex, g_hero_w, g_hero_h, 255);
      vid_clip_off();
   }
   else
   {
      static const char sf[] = "SAVE FILE";
      sh_save_icon(SS_PX + (SS_PW - 32) / 2, SS_PY + 50, 4, C_ITEM, C_CON);
      vid_text(SS_PX + (SS_PW - vid_text_w(sf)) / 2, SS_PY + 82, sf, SH_DIM);
   }
}

/* One of THEIR slots in the preview frame: the full picture once it has
 * arrived, the thumbnail at 2x until then, a plate before that. */
static void shj_panel_their(share_join_t *j, int slot)
{
   const fe_share_entry *x = &j->e[j->ent];
   int s = slot - 1;
   vid_rect(SS_PX + 3, SS_PY + 3, SS_PW, SS_PH, C_SHADOW, 90);
   vid_rect(SS_PX, SS_PY, SS_PW, SS_PH, C_CARD, 255);
   if (j->shot_slot == slot)
      vid_shot_draw(SS_PX + (SS_PW - j->shot_w) / 2,
                    SS_PY + (SS_PH - j->shot_h) / 2, j->shot_w, j->shot_h);
   else if (j->t_th[s] == TH_HAVE)
      vid_image_nearest(SS_PX + (SS_PW - 128) / 2, SS_PY + (SS_PH - 84) / 2 - 8,
                        128, 84, j->tthumb + (size_t)s * SHJ_THUMB_TEXELS,
                        PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_TEX_HEIGHT,
                        PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_HEIGHT);
   else
   {
      const char *m = !j->states_ok ? "needs the same build" :
                      (x->st[s].flags & FE_SHARE_ST_THUMB) &&
                      j->t_th[s] != TH_FAIL ? "preview arriving" : "No preview";
      vid_text(SS_PX + (SS_PW - vid_text_w(m)) / 2,
               vid_text_y_in(SS_PY, SS_PH) - 8, m, SH_DIM);
   }
}

static void shj_draw_their(share_join_t *j)
{
   const fe_share_entry *x = &j->e[j->ent];
   char sub[128], t[96], d1[24], d2[24], kb[16];
   int i, sel, y, by;
   (void)d2;

   rom_display_name(&g_roms[j->rom], t, sizeof(t), 1);
   snprintf(sub, sizeof(sub), "%s  -  %s", t, j->room);
   sh_backdrop(j->rom, 1);
   sh_header("THEIR STATES", sub, j->group, 0, 1);

   /* the save row */
   sel = j->row == 0;
   y = SHT_SAVE_Y;
   by = y + 1;
   if (sel)
   {
      vid_rect(0, by, SHT_LIST_W, 30, C_CON, 56);
      vid_rect(0, by, 4, 30, C_CON, 255);
   }
   if (x->save_size)
   {
      /* The icon plays once when the row is highlighted and holds on the
       * tick; loops its drop while the save is in flight; rests seated. */
      int f = 4;
      if (j->modal == JM_XFER && j->xf_kind == FE_SHARE_OBJ_SAVE)
         f = (j->icon_t / 7) % 5;
      else if (sel)
         f = j->icon_t / 7 > 7 ? 7 : j->icon_t / 7;
      sh_save_icon(22, by + 4, f, sel ? C_SEL : C_ITEM, C_CON);
      vid_text(64, y, "Save file", sel ? C_SEL : C_ITEM);
      if (j->mark[j->ent] & MARK_SAVE)
      {
         sh_tick(64, y + 18, sh_ok_col());
         vid_text(76, y + 14, "replaced yours", sh_ok_col());
      }
      else
      {
         sh_kb(kb, sizeof(kb), x->save_size);
         fe_share_time_text(x->save_time, 0, d1, sizeof(d1));
         snprintf(t, sizeof(t), "%s%s%s", kb, d1[0] ? "  -  " : "", d1);
         vid_text(64, y + 14, t, sel ? C_VALUE : SH_DIM);
      }
   }
   else
   {
      vid_rect(20, by + 3, 36, 24, C_CARD, 200);
      vid_text(64, y, "No save file shared", SH_DIM);
      vid_text(64, y + 14, "states only", SH_DIM);
   }
   vid_rect(20, SHT_SLOT_Y(0) - 2, SHT_LIST_W - 20, 1, C_ACCENT_DK, 110);

   /* the five slots */
   for (i = 0; i < FE_SHARE_SLOTS; i++)
   {
      int have = x->st[i].size != 0, live = have && j->states_ok;
      uint16_t c1, c2;
      y = SHT_SLOT_Y(i);
      by = y + 1;
      sel = j->row == i + 1;
      if (sel)
      {
         vid_rect(0, by, SHT_LIST_W, 30, C_CON, 56);
         vid_rect(0, by, 4, 30, C_CON, 255);
      }
      c1 = !live ? SH_DIM : sel ? C_SEL : C_ITEM;
      c2 = !live ? SH_DIM : sel ? C_VALUE : SH_DIM;
      if (have && j->t_th[i] == TH_HAVE)
         vid_image(20, by + 3, 36, 24,
                   j->tthumb + (size_t)i * SHJ_THUMB_TEXELS,
                   PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_TEX_HEIGHT,
                   PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_HEIGHT,
                   live ? 255 : 110);
      else
         vid_rect(20, by + 3, 36, 24, C_CARD, have ? 255 : 150);
      snprintf(t, sizeof(t), "Slot %d", i + 1);
      vid_text(64, y, t, c1);
      if (!have)
         vid_text(64, y + 14, "empty", SH_DIM);
      else if (j->mark[j->ent] & (1u << (i + 1)))
      {
         sh_tick(64, y + 18, sh_ok_col());
         vid_text(76, y + 14, "got it", sh_ok_col());
      }
      else
      {
         fe_share_time_text(x->st[i].time, 1, d1, sizeof(d1));
         vid_text(64, y + 14, d1[0] ? d1 : "saved", c2);
      }
   }

   /* the panel */
   if (j->row == 0 || !x->st[j->row - 1].size)
   {
      shj_panel_hero(j);
      if (x->save_size)
      {
         /* The size is on the row and the question repeats both dates
          * in full; the runner is the two days, which is what fits. */
         fe_share_time_text(x->save_time, 0, d1, sizeof(d1));
         fe_share_time_text(j->y_save_time, 0, d2, sizeof(d2));
         if (j->y_save)
            snprintf(t, sizeof(t), "Theirs %s   Yours %s",
                     d1[0] ? d1 : "?", d2[0] ? d2 : "?");
         else
            snprintf(t, sizeof(t), "Theirs %s   you have none",
                     d1[0] ? d1 : "?");
         shj_runner(t);
      }
   }
   else
   {
      shj_panel_their(j, j->row);
      sh_kb(kb, sizeof(kb), x->st[j->row - 1].size);
      fe_share_time_text(x->st[j->row - 1].time, 1, d1, sizeof(d1));
      snprintf(t, sizeof(t), "Slot %d   %s   %s", j->row, d1, kb);
      shj_runner(t);
   }
   ov_outline(SS_PX, SS_PY, SS_PW, SS_PH, C_CON, 140);

   /* the line under the panel: what just landed, else why states are grey */
   if (j->note[0])
   {
      sh_tick(SS_PX, 232, sh_ok_col());
      vid_text(SS_PX + 14, 228, j->note, C_SEL);
   }
   else if (!j->states_ok)
      vid_text(SS_PX, 228, "States need the same build.", SH_DIM);
}

static void shj_draw_yours(share_join_t *j)
{
   const fe_share_entry *x = &j->e[j->ent];
   char sub[100], t[96], d1[24];
   int i;
   rom_display_name(&g_roms[j->rom], sub, sizeof(sub), 1);
   sh_backdrop(j->rom, 1);
   sh_header("YOUR SLOTS", sub, j->group, 0, 1);
   snprintf(t, sizeof(t), "Put their Slot %d in:", j->xf_src);
   vid_text(20, 54, t, C_ITEM);
   for (i = 0; i < FE_SHARE_SLOTS; i++)
   {
      int y = SS_ROW_Y(i) + 4, by = y + 1, sel = j->yslot == i + 1;
      if (sel)
      {
         vid_rect(0, by, SHT_LIST_W, 32, C_CON, 56);
         vid_rect(0, by, 4, 32, C_CON, 255);
      }
      if (j->y_th[i] == TH_HAVE)
         vid_image(20, by + 4, 36, 24,
                   j->ythumb + (size_t)i * SHJ_THUMB_TEXELS,
                   PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_TEX_HEIGHT,
                   PSP_STATE_THUMB_WIDTH, PSP_STATE_THUMB_HEIGHT, 255);
      else
         vid_rect(20, by + 4, 36, 24, C_CARD, j->y_have[i] ? 255 : 150);
      snprintf(t, sizeof(t), "Slot %d", i + 1);
      vid_text(64, y, t, sel ? C_SEL : C_ITEM);
      if (j->y_have[i])
      {
         fe_share_time_text(j->y_time[i], 1, d1, sizeof(d1));
         vid_text(64, y + 14, d1[0] ? d1 : "saved", sel ? C_VALUE : SH_DIM);
      }
      else
         vid_text(64, y + 14, "empty", SH_DIM);
   }
   shj_panel_their(j, j->xf_src);
   fe_share_time_text(x->st[j->xf_src - 1].time, 0, d1, sizeof(d1));
   snprintf(t, sizeof(t), "Incoming   their Slot %d   %s", j->xf_src, d1);
   shj_runner(t);
   ov_outline(SS_PX, SS_PY, SS_PW, SS_PH, C_CON, 140);
}

static void shj_draw_modal(share_join_t *j)
{
   const fe_share_entry *x = &j->e[j->ent];
   char l1[96], l2[96], l3[96], nm[64], d1[24], d2[24], kb[16];
   if (j->modal == JM_ASK)
   {
      rom_display_name(&g_roms[j->rom], l3, sizeof(l3), 1);
      short_game_name(l3, nm, sizeof(nm));
      clip_title(nm, 170);
      snprintf(l1, sizeof(l1), "Replace your %s save?", nm);
      fe_share_time_text(j->y_save_time, 0, d2, sizeof(d2));
      fe_share_time_text(x->save_time, 0, d1, sizeof(d1));
      snprintf(l2, sizeof(l2), "Yours is from %s, theirs from %s.",
               d2[0] ? d2 : "an unknown date", d1[0] ? d1 : "an unknown date");
      sh_plate(l1, l2, "The game loads theirs next time you play.",
               "Your current save is kept as a backup.", -1);
   }
   else if (j->modal == JM_XFER)
   {
      unsigned el = share_now_ms() - j->xf_t0;
      sh_kb(kb, sizeof(kb), j->xf_kind == FE_SHARE_OBJ_SAVE
                               ? x->save_size : x->st[j->xf_src - 1].size);
      if (j->xf_kind == FE_SHARE_OBJ_SAVE)
         snprintf(l1, sizeof(l1), "Fetching their save file");
      else
         snprintf(l1, sizeof(l1), "Fetching their Slot %d into your Slot %d",
                  j->xf_src, j->xf_dst);
      if (j->xf_stage > 0)
         snprintf(l2, sizeof(l2), "%s  -  and its preview", kb);
      else if (j->xf_pm >= 80 && el >= 500)
         snprintf(l2, sizeof(l2), "%s  -  about %u s left", kb,
                  (unsigned)((uint64_t)el * (1000 - j->xf_pm) / j->xf_pm /
                             1000) + 1);
      else
         snprintf(l2, sizeof(l2), "%s", kb);
      snprintf(l3, sizeof(l3), j->xf_kind == FE_SHARE_OBJ_SAVE
               ? (j->y_save ? "Yours is replaced when it finishes."
                            : "It is saved when it finishes.")
               : "Your slot changes only when it has all arrived.");
      sh_plate(l1, l2, NULL, l3, (int)j->xf_pm);
   }
   else if (j->modal == JM_LOST)
      sh_plate("Lost the other PSP", j->fail1, j->fail2,
               "Move the PSPs closer, or ask them to share again.", -1);
   else if (j->modal == JM_FAIL)
      sh_plate("Not fetched", j->fail1, j->fail2, NULL, -1);
}

static void shj_draw_footer(share_join_t *j)
{
   const char *h[3];
   int n = 0;
   if (j->modal == JM_ASK)
   {
      h[n++] = VID_GLYPH_X " replace";
      h[n++] = VID_GLYPH_O " keep mine";
   }
   else if (j->modal == JM_XFER)
      h[n++] = VID_GLYPH_O " cancel";
   else if (j->modal == JM_LOST)
   {
      h[n++] = VID_GLYPH_X " try again";
      h[n++] = VID_GLYPH_O " games";
   }
   else if (j->modal == JM_FAIL)
      h[n++] = VID_GLYPH_O " back";
   else if (j->page == JP_YOURS)
   {
      h[n++] = j->y_have[j->yslot - 1] ? VID_GLYPH_X " overwrite"
                                       : VID_GLYPH_X " put it here";
      h[n++] = VID_GLYPH_O " back";
   }
   else
   {
      if (shj_row_ok(j, j->row))
         h[n++] = j->row == 0 ? VID_GLYPH_X " get save" : VID_GLYPH_X " get";
      h[n++] = VID_GLYPH_O " games";
   }
   sh_hints(h, n);
}

/* ---- input ----------------------------------------------------------------- */

static void shj_their_move(share_join_t *j, int dir)
{
   int r = j->row, k;
   for (k = 0; k < FE_SHARE_SLOTS + 1; k++)
   {
      r = (r + dir + FE_SHARE_SLOTS + 1) % (FE_SHARE_SLOTS + 1);
      if (shj_row_ok(j, r))
      {
         if (r != j->row)
         {
            j->row = r;
            j->dwell = 0;
            j->icon_t = 0;
         }
         return;
      }
   }
}

/* "Try again" after the other PSP was lost: find it again in the room
 * (rejoining the room itself if the radio let go of it), and if what it
 * shares is unchanged carry on from the same page with the same item.
 * 0 = linked again, same list; 1 = linked, the list changed; -1 = no. */
static int shj_rejoin(share_join_t *j)
{
   uint32_t crc = j->cl.manifest_crc, size = j->cl.manifest_size;
   fe_share_entry *e = j->e;
   short *local = j->local;
   unsigned char *mark = j->mark;
   unsigned short *view = j->view;
   unsigned n = j->n;
   int rc;
   shj_bg_stop(j);
   if (!share_net_connected())
   {
      sh_wait_frame("GAME DATA", j->group, "Looking for the room again...",
                    NULL);
      share_net_down();
      if (share_net_up(0, j->group) != ADHOC_OK)
         return -1;
   }
   /* keep the list we have in case the sharer's is the same */
   j->e = NULL; j->local = NULL; j->mark = NULL; j->view = NULL;
   rc = shj_connect(j);
   if (rc == 0 && j->cl.manifest_crc == crc && j->cl.manifest_size == size &&
       j->n == n)
   {
      free(j->e); free(j->local); free(j->mark); free(j->view);
      j->e = e; j->local = local; j->mark = mark; j->view = view;
      return 0;
   }
   if (rc != 0)
   {
      free(j->e); free(j->local); free(j->mark); free(j->view);
      j->e = e; j->local = local; j->mark = mark; j->view = view;
      j->n = n;
      return -1;
   }
   free(e); free(local); free(mark); free(view);
   return 1;
}

/* The room list joined a "Share game data" room: everything from here to
 * leaving it.  Opens by itself; there is no menu row for it. */
static void share_join_run(const char *rom_dir, const char *group)
{
   share_join_t *j = (share_join_t *)calloc(1, sizeof(*j));
   char l1[80];
   int rc;
   if (!j)
      return;
   j->file.fd = -1;
   j->rom_dir = rom_dir;
   j->shot_slot = -1;
   snprintf(j->group, sizeof(j->group), "%.8s", group);
   room_label(j->group, j->room, sizeof(j->room));
   j->nonce = (uint32_t)sceKernelGetSystemTimeLow();
   j->dg = (uint8_t *)malloc(FE_SHARE_DGRAM_MAX + 8);
   j->ythumb = (uint16_t *)memalign(16, FE_SHARE_SLOTS * SHJ_THUMB_TEXELS * 2);
   j->tthumb = (uint16_t *)memalign(16, FE_SHARE_SLOTS * SHJ_THUMB_TEXELS * 2);

   snprintf(l1, sizeof(l1), "Joining %s...", j->room);
   sh_wait_frame("GAME DATA", j->group, l1, NULL);
   rc = j->dg ? share_net_up(0, j->group) : ADHOC_ERR_NP_START;
   if (rc != ADHOC_OK)
   {
      sh_notice("GAME DATA", j->group, "Could not join the room.",
                sh_net_error(rc));
      shj_free(j);
      return;
   }
   if (shj_connect(j) != 0)
   {
      shj_connect_notice(j);
      goto leave;
   }
   /* the browser's console, its ROMs, and which of the sharer's games
    * are among them */
   art_free_all();
   browser_state_cache_free();
   j->n_roms = rom_scan(rom_dir, (fe_console_t)g_pcfg.console);
   if (j->n_roms < 0)
      j->n_roms = 0;
   g_sh_rescan = 1;
   g_sw.active = 0;
   fx_reset();
   shj_build_view(j);
   j->page = JP_LIST;

   g_prev_pad = 0xFFFFFFFFu;
   while (g_running && !g_sh_quit)
   {
      unsigned edges = sh_begin();
      int done = 0;              /* leave, once this frame is presented */
      shj_pump(j);

      /* the link itself, when nothing else is reporting it */
      if (j->modal == JM_NONE && !fe_share_client_linked(&j->cl) &&
          j->cl.state == FE_SHARE_CL_ERROR)
      {
         shj_bg_stop(j);
         j->bg_kind = 0;
         snprintf(j->fail1, sizeof(j->fail1), j->cl.err == FE_SHARE_E_CLOSED
                  ? "The other PSP stopped sharing."
                  : "The other PSP is not answering.");
         snprintf(j->fail2, sizeof(j->fail2), "Nothing of yours was changed.");
         j->xf_kind = 0;
         j->modal = JM_LOST;
         fe_evt("share_join_lost err=%s", fe_share_err_name(j->cl.err));
      }

      if (j->modal == JM_XFER)
      {
         shj_xf_step(j);
         if (j->modal == JM_XFER && (edges & PSP_CTRL_CIRCLE))
            shj_xf_cancel(j);
      }
      else if (j->modal == JM_ASK)
      {
         if (edges & PSP_CTRL_CROSS)
            shj_xf_start(j, FE_SHARE_OBJ_SAVE, 0, 0);
         else if (edges)
            j->modal = JM_NONE;
      }
      else if (j->modal == JM_FAIL)
      {
         if (edges & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS))
            j->modal = JM_NONE;
      }
      else if (j->modal == JM_LOST)
      {
         if (edges & PSP_CTRL_CROSS)
         {
            int r;
            vid_overlay_end();          /* shj_rejoin draws its own frames */
            r = shj_rejoin(j);
            fe_evt("share_join_rejoin rc=%d", r);
            (void)sh_begin();
            if (r == 0)
            {
               j->modal = JM_NONE;
               if (j->xf_kind && j->page != JP_LIST)
                  shj_xf_start(j, j->xf_kind, j->xf_src, j->xf_dst);
            }
            else if (r > 0)
            {
               j->n_roms = rom_scan(rom_dir, (fe_console_t)g_pcfg.console);
               shj_build_view(j);
               j->modal = JM_NONE;
               j->page = JP_LIST;
            }
            else
               snprintf(j->fail1, sizeof(j->fail1),
                        "Still no answer from the other PSP.");
         }
         else if (edges & PSP_CTRL_CIRCLE)
         {
            if (j->page == JP_LIST)
               done = 1;                    /* nothing left to do: leave */
            j->modal = JM_NONE;
            j->page = JP_LIST;
            j->idle = 0;
         }
      }
      else if (j->page == JP_LIST)
      {
         if (edges & PSP_CTRL_TRIANGLE)
         {
            if (g_sw.active)
            {
               if (!g_sw.swapped)
                  shj_switch_apply(j);
               g_sw.active = 0;
               fx_reset();
            }
            switch_begin((g_pcfg.console + 1) % FE_CONSOLE_COUNT);
            fe_evt("share_join_switch from=%d to=%d", g_pcfg.console, g_sw.to);
         }
         if (g_sw.active)
         {
            if (switch_step())
               shj_switch_apply(j);
            j->idle = 0;
            edges = 0;
         }
         if (edges & PSP_CTRL_CIRCLE)
            done = 1;
         if (j->view_n > 0)
         {
            int n = j->view_n;
            if (edges & PSP_CTRL_UP)
               { j->cur = (j->cur + n - 1) % n; j->idle = 0; }
            else if (edges & PSP_CTRL_DOWN)
               { j->cur = (j->cur + 1) % n; j->idle = 0; }
            else if (edges & PSP_CTRL_LTRIGGER)
               { j->cur = j->cur >= SHL_ROWS ? j->cur - SHL_ROWS : 0;
                 j->idle = 0; }
            else if (edges & PSP_CTRL_RTRIGGER)
               { j->cur = j->cur + SHL_ROWS < n ? j->cur + SHL_ROWS : n - 1;
                 j->idle = 0; }
            else if (edges & PSP_CTRL_CROSS)
               shj_open_game(j, j->cur);
         }
         if (j->idle < 1000)
            j->idle++;
      }
      else if (j->page == JP_THEIR)
      {
         if (edges)
            j->note[0] = '\0';
         if (edges & PSP_CTRL_CIRCLE)
         {
            shj_bg_stop(j);
            j->page = JP_LIST;
            j->idle = IDLE_HERO;         /* its hero is already decoded */
         }
         else if (edges & PSP_CTRL_UP)
            shj_their_move(j, -1);
         else if (edges & PSP_CTRL_DOWN)
            shj_their_move(j, +1);
         else if ((edges & PSP_CTRL_CROSS) && shj_row_ok(j, j->row))
         {
            if (j->row == 0)
            {
               if (j->y_save)
                  j->modal = JM_ASK;
               else
                  shj_xf_start(j, FE_SHARE_OBJ_SAVE, 0, 0);
            }
            else
            {
               j->xf_src = j->row;
               j->yslot = j->row;
               j->page = JP_YOURS;
            }
         }
         if (j->page == JP_THEIR && j->modal == JM_NONE)
         {
            if (j->dwell < 1000)
               j->dwell++;
            shj_bg_step(j);
         }
         j->icon_t++;
      }
      else if (j->page == JP_YOURS)
      {
         if (edges & PSP_CTRL_CIRCLE)
            j->page = JP_THEIR;
         else if (edges & PSP_CTRL_UP)
            j->yslot = j->yslot > 1 ? j->yslot - 1 : FE_SHARE_SLOTS;
         else if (edges & PSP_CTRL_DOWN)
            j->yslot = j->yslot < FE_SHARE_SLOTS ? j->yslot + 1 : 1;
         else if (edges & PSP_CTRL_CROSS)
            shj_xf_start(j, FE_SHARE_OBJ_STATE, j->xf_src, j->yslot);
         /* the incoming picture keeps arriving while the slot is chosen */
         if (j->page == JP_YOURS && j->modal == JM_NONE)
            shj_bg_step(j);
      }

      if (j->page == JP_LIST)
         shj_draw_list(j);
      else if (j->page == JP_THEIR)
         shj_draw_their(j);
      else
         shj_draw_yours(j);
      if (j->modal != JM_NONE)
      {
         shj_draw_modal(j);
         shj_draw_footer(j);
      }
      else if (j->page != JP_LIST)
         shj_draw_footer(j);
      if (j->page == JP_LIST)
         switch_draw();
      sh_end();
      if (done)
         break;
   }
   g_sw.active = 0;
   fx_reset();

leave:
   /* Leaving the room: the marks go with it (they live in j). */
   fe_evt("share_join_leave rx_ok=%u rx_bad=%u dup=%u gets=%u retries=%u",
          j->cl.rx_ok, j->cl.rx_bad, j->cl.rx_dup, j->cl.tx_get,
          j->cl.retries);
   sh_wait_frame("GAME DATA", j->group, "Leaving the room...", NULL);
   shj_drop_tmps(j);
   fe_share_client_bye(&j->cl);
   sceKernelDelayThread(60000);
   share_net_down();
   shj_free(j);
   art_free_all();
   g_ov_shot_slot = -1;
   g_prev_pad = 0xFFFFFFFFu;
}

#pragma GCC pop_options
