/* ------------------------------------------------------------------- misc */

static int find_first_rom(char *out, size_t out_sz)
{
   SceUID d = sceIoDopen(ROM_DIR);
   SceIoDirent ent;
   int found = 0;

   if (d < 0)
      return -1;
   memset(&ent, 0, sizeof(ent));
   while (sceIoDread(d, &ent) > 0)
   {
      size_t n = strlen(ent.d_name);
      if (n > 4 && strcasecmp(ent.d_name + n - 4, ".gba") == 0)
      {
         if (psp_rom_path_join(out, out_sz, ROM_DIR, ent.d_name) == 0)
         {
            found = 1;
            break;
         }
      }
      memset(&ent, 0, sizeof(ent));
   }
   sceIoDclose(d);
   return found ? 0 : -1;
}

static int make_suffixed_path(const char *rom, const char *suffix,
                              char *out, size_t out_sz)
{
   return psp_rom_path_suffix(out, out_sz, rom, suffix);
}

/* FOLDERS (4.6.0-beta.4, psp/sidecar_dirs.h): main's `folders` = 1: a
 * game's battery save is <app>/saves/<its path under roms/>, its states
 * <app>/states/..., moved
 * there from beside the ROM the first time the game is opened.  On for
 * every player path (the browser, an XMB launch, a playable build); a
 * harness that boots a ROM by itself (rom = / the first ROM) keeps the
 * files beside the ROM unless it says `folders = 1`, because the e2e and
 * rig drivers stage and read back roms/<game>.sav.  A Lab run is unaffected
 * either way: lab_sidecars moves both files to its work folder. */

/* save_path / state_path hold the files beside the ROM (the legacy layout);
 * when the folders apply, make the new ones, move the game's files there
 * once (verified copy, then the originals removed), and use them -- or keep
 * the legacy ones if anything failed.  `lab_save` is save_path before
 * lab_sidecars: when they differ, a Lab work folder took the paths and they
 * are left alone.  A failure only ever leaves the legacy paths. */
static void folders_settle(int folders, const char *rom_path,
                           const char *save_suffix,
                           fe_console_t console, char *save_path,
                           size_t save_sz, char *state_path, size_t state_sz,
                           const char *lab_save)
{
   char mir[PSP_FILE_PATH_CAP], ns[PSP_FILE_PATH_CAP], nst[PSP_FILE_PATH_CAP];
   const char *rel, *use;
   int rs, rt;
   if (!folders || strcmp(lab_save, save_path) != 0)
      return;
   rel = sc_rel_of(ROM_DIR, rom_path);
   if (!rel)
   {
      fe_evt("folders rom_outside_roms keep_beside=%s", save_path);
      return;
   }
   if (sc_mirror(mir, sizeof(mir), g_dir_base, SC_SAVES_DIR, rel) != 0 ||
       make_suffixed_path(mir, save_suffix, ns, sizeof(ns)) != 0 ||
       sc_mirror(mir, sizeof(mir), g_dir_base, SC_STATES_DIR, rel) != 0 ||
       make_suffixed_path(mir, psp_state_slot1_suffix(console), nst,
                          sizeof(nst)) != 0)
   {
      fe_evt("folders path_too_long keep_beside=%s", save_path);
      return;
   }
   rs = sc_settle(SC_FS_PSP, g_dir_base, SC_GROUP_SAVE, save_path, ns,
                  &use, NULL);
   if (use == ns)
      snprintf(save_path, save_sz, "%s", ns);
   rt = sc_settle(SC_FS_PSP, g_dir_base, SC_GROUP_STATES, state_path, nst,
                  &use, NULL);
   if (use == nst)
      snprintf(state_path, state_sz, "%s", nst);
   fe_evt("folders save_mig=%d state_mig=%d save=%s state=%s", rs, rt,
          save_path, state_path);
   (void)rs;
   (void)rt;
}

static fe_console_t console_from_rom_path(const char *path)
{
   const char *dot = path ? strrchr(path, '.') : NULL;
   if (dot && strcasecmp(dot, ".gbc") == 0)
      return FE_CONSOLE_GBC;
   if (dot && strcasecmp(dot, ".gb") == 0)
      return FE_CONSOLE_GB;
   return FE_CONSOLE_GBA;
}

static __attribute__((noinline)) void dump_frame_bmp(void)
{
   size_t pitch;
   const uint16_t *pix = fe_host_last_frame(&pitch);
   unsigned w = g_last_w, h = g_last_h;
   char path[176];
   /* Media Engine mode: the core buffer is never written (capture mode skips
    * the render) — dump the frame the ME actually presented instead, so
    * screenshots show what the screen shows. */
   {
      const uint16_t *me = NULL;
      unsigned mp = 0;
      if (me_rend_presenting(&me, &mp))
      {
         pix   = me;
         pitch = mp * 2;   /* fe_host_last_frame pitch is BYTES */
         w = FE_GBA_WIDTH;
         h = FE_GBA_HEIGHT;
      }
   }
   if (!pix)
      return;
   snprintf(path, sizeof(path), "%s/frame_%06u.bmp", LOG_DIR,
            fe_host_frame_count());
   /* Decode the core buffer with the layout the core was BUILT to emit
    * (ADR-0039), or every dumped BMP — and the harness comparisons built on
    * them — comes out with R and B swapped. */
#ifdef USE_PSP_RGB565_FORMAT
   if (fe_bmp_write_psp565(path, pix, w, h, pitch) == 0)
#else
   if (fe_bmp_write_rgb565(path, pix, w, h, pitch) == 0)
#endif
      fe_evt("frame_dump file=%s", path);
}

/* The frame a state's previews are made from: the one the player last saw
 * (the core's last frame, or the ME's READY stage when the renderer is up),
 * its own size -- 240x160 for GBA, 160x144 for GB/GBC -- and its pitch in
 * BYTES.  0 = there is one. */
static int state_preview_src(const uint16_t **pix, size_t *pitch,
                             unsigned *sw, unsigned *sh)
{
   const uint16_t *me = NULL;
   unsigned me_pitch = 0;
   *pix = fe_host_last_frame(pitch);
   *sw = g_last_w ? g_last_w : FE_GBA_WIDTH;
   *sh = g_last_h ? g_last_h : FE_GBA_HEIGHT;
   if (me_rend_presenting(&me, &me_pitch))
   {
      *pix = me;
      *pitch = (size_t)me_pitch * 2;
      *sw = FE_GBA_WIDTH;
      *sh = FE_GBA_HEIGHT;
   }
   return (!*pix || !*sw || !*sh || *pitch < *sw * sizeof(uint16_t)) ? -1 : 0;
}

/* ---- THE PLAYER'S SCREENSHOT (GitHub issue #13) ---------------------------
 *
 * The Screenshot shortcut (L+R+SELECT unless remapped).  The harness dumps
 * (dump_at, dump_every, the autopilot's, the marker) keep dump_frame_bmp and
 * log/frame_<n>.bmp: rigs compare those by name.
 *
 *   name    <game>_<YYYY-MM-DD>_<HH-MM-SS>.png  (shot_name.h), from the ROM's
 *           file name and the console's own clock -- sceRtc local time, never
 *           time() (uptime on real firmware, memory rtc-wallclock-psp); a
 *           second shot in the same second gets _2, _3, ...
 *   where   Settings > Screenshot folder: <dev>:/PICTURE/GBAdhoc/ (default),
 *           the XMB Photo column's own folder -- on the device the EBOOT runs
 *           from, so a PSP Go running from internal storage writes ef0: -- or
 *           Screenshots/ beside the EBOOT (since 4.6.0-beta.4, issue #31;
 *           4.x up to beta.3 put the "app" choice in log/).
 *   what    Settings > Screenshot picture (5.0):
 *           "As displayed" (default): the 480x272 screen as the player sees
 *           it -- scale, smoothing, screen effect, colour, ambient bars, GB
 *           palette -- read from the GE's display buffer in VRAM, without
 *           the OSD, the rewind HUD or a menu (video_psp.c "AS DISPLAYED").
 *           When the newest picture has an overlay on it, the shot waits for
 *           the next one (vid_cap_arm; shot_ask_again raises the chord
 *           again, so the loop asks next time), at most SHOT_WAIT_LOOPS
 *           loops, after which it saves the game picture instead
 *           (EVT screenshot ... src=game).
 *           "Game only": the game's own frame, 240x160 or 160x144, in its
 *           colour mode (state_preview_src), as before 5.0.
 *   format  Settings > Screenshot format: PNG (default) or BMP (24-bit:
 *           115 KB for the game picture, about 390 KB for the screen).
 *   memory  NONE for "As displayed": fe_png_stream and shot_write_bmp_rows
 *           read VRAM row by row with stack buffers only (~24 KiB of the
 *           main thread's 256 KiB).  "Game only" PNG needs ~200 KiB of work
 *           memory and the heap is empty once a ROM is in (memory
 *           heap-exhausted-by-design), so it borrows the netdrv arena
 *           (netdrv_arena_borrow) for the encode: reserved at boot for
 *           wireless and idle whenever no session is up, so no heap of its
 *           own.  Arena lent to a session, or never reserved: that shot is
 *           a BMP (toast "Screenshot saved (as BMP)").
 *   cost    one-off, on the emulation thread, with the power switch held
 *           off for the write (EVT screenshot ... us=). */
static char     g_shot_rom[PSP_FILE_PATH_CAP];   /* the running game's path */
/* The screenshot's code runs once per shot: size-optimised, because every
 * byte of it sits between layout_pad_cold and the per-frame code the text
 * pad holds in place (docs/LAYOUT-PINNING.md, "Keeping the pads"). */
#define SHOT_SMALL __attribute__((optimize("Os")))
/* Loops an "As displayed" shot has waited for a picture with nothing drawn
 * over it (0 = none waiting).  Two seconds of loops without one (a menu or
 * an overlay that stays up) and the game picture is saved instead. */
static unsigned g_shot_wait;
#define SHOT_WAIT_LOOPS 120u
/* fe_host_frame_count() when the waiting shot was asked for: a count below
 * it means the game it waited in has been left (a quit to the game list in
 * the same process starts the count again), so nothing it held is used. */
static unsigned g_shot_from;
/* Raises the screenshot chord (g_dump_chord_pending) again, so the main
 * loop calls player_screenshot next time. */
static void shot_ask_again(void);

static int shot_fd_write(void *ctx, const void *d, size_t n)
{
   return sceIoWrite(*(SceUID *)ctx, d, (SceSize)n) == (int)n ? 0 : -1;
}

/* The folder for the setting, created as needed.  0 = it exists. */
SHOT_SMALL static int shot_folder(char *dir, size_t cap)
{
   SceIoStat st;
   if (g_pcfg.shot_folder == PCFG_SHOT_APP)
      /* Its own folder, not log/ among the diagnostics (issue #31). */
      snprintf(dir, cap, "%s/Screenshots", g_dir_base);
   else
   {
      /* "ms0:/PSP/GAME/X" -> "ms0:"; no device prefix -> ms0: */
      const char *colon = strchr(g_dir_base, ':');
      int dev = colon && colon - g_dir_base < 8 ? (int)(colon - g_dir_base) + 1
                                                : 0;
      snprintf(dir, cap, "%.*s/PICTURE", dev ? dev : 4,
               dev ? g_dir_base : "ms0:");
      sceIoMkdir(dir, 0777);
      snprintf(dir + strlen(dir), cap - strlen(dir), "/GBAdhoc");
   }
   sceIoMkdir(dir, 0777);
   return sceIoGetstat(dir, &st) >= 0 ? 0 : -1;
}

SHOT_SMALL static int shot_write_png(const char *path, const uint16_t *pix,
                                     unsigned w, unsigned h, size_t pitch,
                                     fe_png_info *info, uint8_t *work,
                                     size_t work_bytes)
{
   SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   int rc, crc;
   if (fd < 0)
      return -1;
   rc = fe_png_encode(pix, w, h, pitch,
#ifdef USE_PSP_RGB565_FORMAT
                      FE_PNG_PSP5650,
#else
                      FE_PNG_RGB565,
#endif
                      work, work_bytes, shot_fd_write, &fd,
                      info);
   crc = sceIoClose(fd);
   if (rc != 0 || crc < 0)
   {
      sceIoRemove(path);        /* never leave half a picture behind */
      return -1;
   }
   return 0;
}

/* The screen as a PNG: fe_png_stream reads the rows in place, no heap. */
SHOT_SMALL static int shot_stream_png(const char *path, const uint16_t *pix, unsigned w,
                           unsigned h, size_t pitch, fe_png_info *info)
{
   SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   int rc, crc;
   if (fd < 0)
      return -1;
   rc = fe_png_stream(pix, w, h, pitch, FE_PNG_PSP5650, shot_fd_write, &fd,
                      info);
   crc = sceIoClose(fd);
   if (rc != 0 || crc < 0)
   {
      sceIoRemove(path);
      return -1;
   }
   return 0;
}

static void shot_le32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A 24-bit BMP of a PSP 5650 picture, written from the stack: rows are read
 * in place, bottom up, four at a time (fe_bmp_write_psp565 takes a row from
 * the heap and a stdio buffer, and the heap is empty in a game).  w <= 480
 * and a multiple of 4 (no row padding): the screen.  0 = written. */
SHOT_SMALL static int shot_write_bmp_rows(const char *path, const uint16_t *pix,
                               unsigned w, unsigned h, size_t pitch)
{
   uint8_t buf[4 * 480 * 3];
   uint8_t hdr[54];
   uint32_t img = (uint32_t)w * 3u * h;
   unsigned y, n = 0, x;
   SceUID fd;
   int bad = 0;

   if (w > 480 || (w & 3u))
      return -1;
   fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd < 0)
      return -1;
   memset(hdr, 0, sizeof(hdr));
   hdr[0] = 'B'; hdr[1] = 'M';
   shot_le32(hdr + 2, 54u + img);
   hdr[10] = 54; hdr[14] = 40;
   shot_le32(hdr + 18, w);
   shot_le32(hdr + 22, h);
   hdr[26] = 1; hdr[28] = 24;
   shot_le32(hdr + 34, img);
   bad |= sceIoWrite(fd, hdr, sizeof(hdr)) != (int)sizeof(hdr);
   for (y = 0; y < h && !bad; y++)
   {
      const uint16_t *r = (const uint16_t *)(const void *)
         ((const uint8_t *)pix + (size_t)(h - 1u - y) * pitch);
      uint8_t *d = buf + n * w * 3u;
      for (x = 0; x < w; x++, d += 3)
      {
         unsigned c = r[x], cr = c & 31u, cg = (c >> 5) & 63u, cb = c >> 11;
         d[0] = (uint8_t)((cb << 3) | (cb >> 2));
         d[1] = (uint8_t)((cg << 2) | (cg >> 4));
         d[2] = (uint8_t)((cr << 3) | (cr >> 2));
      }
      if (++n == 4u || y + 1u == h)
      {
         bad |= sceIoWrite(fd, buf, n * w * 3u) != (int)(n * w * 3u);
         n = 0;
      }
   }
   if (sceIoClose(fd) < 0 || bad)
   {
      sceIoRemove(path);
      return -1;
   }
   return 0;
}

#ifdef GPSP_KEEP_TELEMETRY
/* critique-3 SF6: netdrv_arena_borrow/_return and netdrv_create share one
 * busy flag with no lock, so the screenshot's borrow is only safe on the
 * thread that starts sessions (net_start_np -> fe_np_start -> netdrv_create).
 * Both run on the main thread today.  The diagnostic builds check it: the
 * thread of the last session start, and an EVT if a borrow happens elsewhere.
 * Nothing in a release (no code, no data). */
static SceUID g_np_start_tid = -1;
#endif

SHOT_SMALL static void player_screenshot(void)
{
   const uint16_t *pix = NULL;
   size_t pitch = 0;
   unsigned w = 0, h = 0, seq, t0, us;
   char dir[sizeof(p_log_dir) + 16], title[SHOT_TITLE_MAX + 1];
   char name[SHOT_TITLE_MAX + 48];
   char path[PSP_FILE_PATH_CAP];
   fe_png_info info;
   time_t now;
   struct tm tmv;
   int png, rc = -1, screen = 0;
   SceIoStat st;
   size_t work_bytes = 0;
   uint8_t *work = NULL;

   memset(&info, 0, sizeof(info));
   if (g_pcfg.shot_source == PCFG_SHOT_DISPLAYED)
   {
      if (g_shot_wait && fe_host_frame_count() < g_shot_from)
         g_shot_wait = 0;          /* asked for in a game since left */
      if (!g_shot_wait)
      {
         vid_cap_reset();          /* nothing held from an earlier shot */
         g_shot_from = fe_host_frame_count();
      }
      pix = vid_cap_frame(&w, &h, &pitch);
      if (!pix && g_shot_wait < SHOT_WAIT_LOOPS && vid_cap_arm())
      {
         g_shot_wait++;
         shot_ask_again();          /* the next picture, next loop */
         return;
      }
      screen = pix != NULL;        /* else the game picture (EVT src=game) */
      g_shot_wait = 0;
   }
   if (!screen && state_preview_src(&pix, &pitch, &w, &h) != 0)
   {
      osd_toast("Screenshot failed: no picture yet");
      return;
   }
   pwr_hold("screenshot");
   t0 = sceKernelGetSystemTimeLow();
   if (g_pcfg.shot_format == PCFG_SHOT_PNG && !screen)
   {
      /* Lent only while no driver lives in it; handed back below, before
       * this thread can reach anything that starts a session. */
      work_bytes = fe_png_work_bytes(w, h);
      work = (uint8_t *)netdrv_arena_borrow(work_bytes);
#ifdef GPSP_KEEP_TELEMETRY
      {
         SceUID tid = sceKernelGetThreadId();
         if (g_np_start_tid >= 0 && tid != g_np_start_tid)
            fe_evt("arena_thread_mismatch borrow=%08x np_start=%08x",
                   (unsigned)tid, (unsigned)g_np_start_tid);
      }
#endif
   }
   png = screen ? g_pcfg.shot_format == PCFG_SHOT_PNG : work != NULL;
   now = psp_local_wallclock();     /* local time, held as if UTC */
   memset(&tmv, 0, sizeof(tmv));
   gmtime_r(&now, &tmv);
   shot_title_from_rom(g_shot_rom, title, sizeof(title));
   path[0] = '\0';
   if (shot_folder(dir, sizeof(dir)) == 0)
      for (seq = 1; seq < 100; seq++)
      {
         if (shot_file_name(name, sizeof(name), title, tmv.tm_year + 1900,
                            tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                            tmv.tm_min, tmv.tm_sec, seq,
                            png ? "png" : "bmp") != 0 ||
             psp_rom_path_join(path, sizeof(path), dir, name) != 0)
         {
            path[0] = '\0';
            break;
         }
         if (sceIoGetstat(path, &st) < 0)
            break;                  /* a free name */
         path[0] = '\0';
      }
   if (path[0])
   {
      if (screen)
         rc = png ? shot_stream_png(path, pix, w, h, pitch, &info)
                  : shot_write_bmp_rows(path, pix, w, h, pitch);
      else if (png)
         rc = shot_write_png(path, pix, w, h, pitch, &info, work,
                             work_bytes);
      else
#ifdef USE_PSP_RGB565_FORMAT
         rc = fe_bmp_write_psp565(path, pix, w, h, pitch);
#else
         rc = fe_bmp_write_rgb565(path, pix, w, h, pitch);
#endif
   }
   netdrv_arena_return(work);
   us = sceKernelGetSystemTimeLow() - t0;
   pwr_release();
   FE_EVT_ONLY(us);
   fe_evt("screenshot file=%s fmt=%s src=%s rc=%d %ux%u colors=%u depth=%u "
          "bytes=%u us=%u", path[0] ? path : "(none)", png ? "png" : "bmp",
          screen ? "screen" : "game", rc, w, h, info.colors, info.bit_depth,
          (unsigned)info.bytes, us);
   if (rc == 0)
      osd_toast(png || g_pcfg.shot_format == PCFG_SHOT_BMP
                   ? "Screenshot saved" : "Screenshot saved (as BMP)");
   else
      osd_toast("Screenshot FAILED (is the Memory Stick full?)");
}

/* Save a compact companion image for the browser's per-slot state shelf.
 * The preview is deliberately separate from the savestate so old states
 * remain loadable and a failed thumbnail write cannot invalidate a save. */
static int state_thumb_write(const char *state_file)
{
   size_t pitch;
   const uint16_t *pix;
   uint16_t thumb[PSP_STATE_THUMB_WIDTH * PSP_STATE_THUMB_HEIGHT];
   unsigned char hdr[PSP_STATE_THUMB_HEADER_SIZE] = {0};
   char path[PSP_STATE_THUMB_PATH_CAP];
   SceUID fd;
   unsigned x, y, x0, dw;
   size_t bytes = sizeof(thumb);
   unsigned sw, sh;

   if (state_preview_src(&pix, &pitch, &sw, &sh) != 0 ||
       psp_state_thumb_path(path, sizeof(path), state_file) != 0)
      return -1;

   /* The thumbnail is 3:2 like a GBA screen.  A narrower frame keeps its
    * shape and is pillarboxed: a GB/GBC screen becomes 47x42, centred on
    * black, rather than stretched.  0 is black in either RGB565 order. */
   dw = PSP_STATE_THUMB_WIDTH;
   if (sw * PSP_STATE_THUMB_HEIGHT < dw * sh)
      dw = (sw * PSP_STATE_THUMB_HEIGHT + sh / 2) / sh;
   x0 = (PSP_STATE_THUMB_WIDTH - dw) / 2;
   memset(thumb, 0, sizeof(thumb));
   for (y = 0; y < PSP_STATE_THUMB_HEIGHT; y++)
   {
      unsigned sy = y * sh / PSP_STATE_THUMB_HEIGHT;
      const uint16_t *src = (const uint16_t *)((const unsigned char *)pix +
                                              (size_t)sy * pitch);
      for (x = 0; x < dw; x++)
         thumb[y * PSP_STATE_THUMB_WIDTH + x0 + x] = src[x * sw / dw];
   }
   memcpy(hdr, PSP_STATE_THUMB_MAGIC, 4);
   hdr[4] = PSP_STATE_THUMB_WIDTH;
   hdr[6] = PSP_STATE_THUMB_HEIGHT;
   hdr[8] = PSP_STATE_THUMB_WIDTH;
   fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd < 0)
      return -1;
   if (sceIoWrite(fd, hdr, sizeof(hdr)) != sizeof(hdr) ||
       sceIoWrite(fd, thumb, bytes) != (int)bytes)
   {
      sceIoClose(fd);
      sceIoRemove(path);
      return -1;
   }
   sceIoClose(fd);
   return 0;
}

/* THE FULL-SIZE PREVIEW (state_slots.h; docs/UI-OVERLAY.md section 13):
 * `<state>.shot`, the same frame as the .thumb at its own size, rows at the
 * page texture's stride of 256 so the page reads the body in one pass.  Rows
 * go out SHOT_ROWS_PER_WRITE at a time through a 4 KiB stack buffer (the
 * padding past `sw` is zero): 20 writes for a GBA frame, nothing allocated.
 * Any short write removes the file -- a torn .shot is never left to be
 * drawn. */
#define SHOT_ROWS_PER_WRITE 8
static int state_shot_write(const char *state_file)
{
   size_t pitch;
   const uint16_t *pix;
   uint16_t rows[SHOT_ROWS_PER_WRITE * PSP_STATE_SHOT_STRIDE];
   unsigned char hdr[PSP_STATE_THUMB_HEADER_SIZE];
   char path[PSP_STATE_SHOT_PATH_CAP];
   SceUID fd;
   unsigned sw, sh, y, k, n;
   int ok = 1;

   if (state_preview_src(&pix, &pitch, &sw, &sh) != 0 ||
       sw > PSP_STATE_SHOT_MAX_W || sh > PSP_STATE_SHOT_MAX_H ||
       psp_state_shot_path(path, sizeof(path), state_file) != 0)
      return -1;
   psp_state_shot_header(hdr, sw, sh);
   fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd < 0)
      return -1;
   if (sceIoWrite(fd, hdr, sizeof(hdr)) != (int)sizeof(hdr))
      ok = 0;
   memset(rows, 0, sizeof(rows));
   for (y = 0; ok && y < sh; y += n)
   {
      int bytes;
      n = sh - y < SHOT_ROWS_PER_WRITE ? sh - y : SHOT_ROWS_PER_WRITE;
      for (k = 0; k < n; k++)
         memcpy(rows + k * PSP_STATE_SHOT_STRIDE,
                (const unsigned char *)pix + (size_t)(y + k) * pitch,
                sw * sizeof(uint16_t));
      bytes = (int)(n * PSP_STATE_SHOT_STRIDE * sizeof(uint16_t));
      if (sceIoWrite(fd, rows, (SceSize)bytes) != bytes)
         ok = 0;
   }
   sceIoClose(fd);
   if (!ok)
   {
      sceIoRemove(path);
      return -1;
   }
   return 0;
}

static int state_save_slot_io(const char *slot1_path, unsigned slot)
{
   char path[PSP_FILE_PATH_CAP];
   unsigned t0 = sceKernelGetSystemTimeLow(), t1, t2, t3;
   int thumb_rc, shot_rc;
   if (psp_state_path_for_slot(path, sizeof(path), slot1_path, slot) != 0 ||
       fe_host_state_save(path) != 0)
      return -1;
   t1 = sceKernelGetSystemTimeLow();
   thumb_rc = state_thumb_write(path);
   if (thumb_rc != 0)
   {
      char thumb[PSP_STATE_THUMB_PATH_CAP];
      if (psp_state_thumb_path(thumb, sizeof(thumb), path) == 0)
         sceIoRemove(thumb); /* never leave a stale preview after overwrite */
   }
   t2 = sceKernelGetSystemTimeLow();
   shot_rc = state_shot_write(path);
   if (shot_rc != 0)
   {
      char shot[PSP_STATE_SHOT_PATH_CAP];
      if (psp_state_shot_path(shot, sizeof(shot), path) == 0)
         sceIoRemove(shot);  /* nor a stale picture: the 3.1 fallback shows */
   }
   t3 = sceKernelGetSystemTimeLow();
   FE_EVT_ONLY(t0); FE_EVT_ONLY(t1); FE_EVT_ONLY(t2); FE_EVT_ONLY(t3);
   FE_EVT_ONLY(thumb_rc); FE_EVT_ONLY(shot_rc);
   fe_evt("state_save slot=%u state_us=%u thumb=%d thumb_us=%u shot=%d "
          "shot_us=%u", slot, t1 - t0, thumb_rc, t2 - t1, shot_rc, t3 - t2);
   return 0;
}

/* pwr_hold: see power_cb -- this runs outside the loop's park point. */
static int state_save_slot(const char *slot1_path, unsigned slot)
{
   int rc;
   pwr_hold("state_save");
#ifdef REWIND
   /* The state is the frame on screen, not the old frame the machine is
    * re-running for a rewind (which may be the middle of an old save
    * sequence: loaded later, it would finish that over a newer save). */
   (void)psp_rewind_land("state_save", 0);
#endif
   rc = state_save_slot_io(slot1_path, slot);
   pwr_release();
   return rc;
}

/* The ME checksums every frame it renders.  Logging it at RETIRE gives one
 * value per rendered frame, independent of when it is presented -- the only
 * oracle that can tell "same-frame drew something different" apart from
 * "same-frame drew the same thing earlier".  vhash samples at a fixed point
 * in the frame loop and therefore conflates the two. */
static void mer_note_sum(void)
{
   if (g_mer_pending >= 0)
      g_mer_stage_sum[g_mer_pending] = me_host_result();
   if (g_vhash_on)
      fe_evt("mesum f=%u s=%08x", fe_host_frame_count(), me_host_result());
}

#ifdef GPSP_PERF_RIG
/* Guest-state oracle (harness `shash = 1`): one line per frame hashing what
 * the GUEST can observe -- r0-r15/CPSR, IWRAM and EWRAM data (not the SMC tag
 * halves), I/O, palette, OAM, VRAM -- plus the rolling audio hash so far.
 * Two builds fed the same state and inputs must print identical lines; the
 * first line that differs names the frame, and the field names what diverged.
 * Built for the paged-vs-resident ROM differential (ROM_BUFFER_SIZE 15/32). */
static int g_shash_on;
/* FNV-style over 32-bit words, not bytes: ~97K steps a frame instead of
 * ~388K, so the oracle costs well under a millisecond on a PSP and does not
 * push Unbound's frame past its budget.  Every region hashed is a multiple of
 * 4 bytes and 4-aligned.  Change it and every stored shash.txt is void. */
static uint32_t shash_fnv(const void *p, size_t n)
{
   const uint32_t *w = (const uint32_t *)p;
   uint32_t h = 2166136261u;
   size_t i;
   for (i = 0; i < n / 4; i++)
      h = (h ^ w[i]) * 16777619u;
   return h;
}
static long g_shash_dump_from, g_shash_dump_to;
static void shash_dump(void)
{
   /* `shash_dump_from`/`shash_dump_to`: raw guest memory for the frames the
    * hashes disagree on -- IWRAM data, EWRAM data, I/O, reg[0..63]. */
   char path[176];
   FILE *fp;
   unsigned f = fe_host_frame_count();
   if (!g_shash_dump_to || f < (unsigned)g_shash_dump_from ||
       f > (unsigned)g_shash_dump_to)
      return;
   snprintf(path, sizeof(path), "%s/gs_%06u.bin", LOG_DIR, f);
   fp = fopen(path, "wb");
   if (!fp)
      return;
   fwrite(iwram + 0x8000, 1, 0x8000, fp);
   fwrite(ewram, 1, 0x40000, fp);
   fwrite(io_registers, 1, 0x400, fp);
   fwrite(reg, 4, 64, fp);
   fwrite(iwram, 1, 0x8000, fp);          /* SMC tag half, for context */
   fclose(fp);
}
/* Written straight to log/shash.txt from this thread, NOT through fe_evt:
 * the event ring is lossy by design (it drops whole lines when the writer
 * thread falls behind), and a differential oracle with holes cannot name the
 * first differing frame. */
static FILE *g_shash_fp;
static char  g_shash_buf[64 * 1024];
static unsigned g_shash_lines;
static char  g_shash_id[112];      /* "run_id=.. arm=.. eboot_crc=.." */
/* A freeze leaves the stdio buffer unwritten, so flush on a cadence: every
 * 120 lines is ~13 KB, one Memory Stick write every two seconds. */
#define SHASH_FLUSH_EVERY 120u
/* Opened at boot when `shash = 1` (main), not at the first frame: a FILE
 * opened mid-game holds a newlib lock object for as long as it stays open,
 * and this one stays open across the in-process return to the game list, so
 * its first game's census would read it as a leak (docs/INPROC-RETURN.md).
 * The lazy open in shash_frame is the fallback if the boot one failed. */
static void shash_open(void)
{
   if (!g_shash_fp)
   {
      char path[176];
      snprintf(path, sizeof(path), "%s/shash.txt", LOG_DIR);
      g_shash_fp = fopen(path, "w");
      if (!g_shash_fp)
         return;
      setvbuf(g_shash_fp, g_shash_buf, _IOFBF, sizeof(g_shash_buf));
      /* Identity first, so a file separated from its log still says which
       * run, arm and binary wrote it. */
      fprintf(g_shash_fp, "# shash v3 %s\n", g_shash_id);
   }
}

static void shash_frame(void)
{
   shash_dump();
   shash_open();
   if (!g_shash_fp)
      return;
   /* c= : this frame's retro_run microseconds (fe_host_core_prof delta), so
    * one run yields per-scene CPU cost beside the oracle (link bench,
    * docs/GBA-LINK-FEASIBILITY.md).  The comparators ignore it. */
   unsigned c_calls = 0; uint32_t c_tot = 0, c_max = 0;
   static uint32_t c_prev;
   fe_host_core_prof(&c_calls, &c_tot, &c_max);
   fprintf(g_shash_fp, "f=%u c=%u a=%08x n=%u r=%08x pc=%08x i=%08x e=%08x "
           "io=%08x p=%08x o=%08x v=%08x\n",
           fe_host_frame_count(), (unsigned)(c_tot - c_prev),
           (unsigned)plat_audio_hash(),
           (unsigned)plat_audio_sample_count(),
           (unsigned)shash_fnv(reg, 17 * 4), (unsigned)reg[15],
           (unsigned)shash_fnv(iwram + 0x8000, 0x8000),
           (unsigned)shash_fnv(ewram, 0x40000),
           (unsigned)shash_fnv(io_registers, 0x400),
           (unsigned)shash_fnv(palette_ram, 0x400),
           (unsigned)shash_fnv(oam_ram, 0x400),
           (unsigned)shash_fnv(vram, 1024 * 96));
   c_prev = c_tot;
   if (++g_shash_lines % SHASH_FLUSH_EVERY == 0)
      fflush(g_shash_fp);
}
static void shash_close(void)
{
   if (g_shash_fp)
      fclose(g_shash_fp);
   g_shash_fp = NULL;
}
#endif

/* ADR-0033: per-frame hash of the CORE's output buffer, taken upstream of the
 * GU blit (fe_host_last_frame is the pointer retro_video_refresh handed us).
 * This is the video regression oracle: a renderer change that alters ONE
 * pixel changes the hash.  FNV-1a over the 240x160 halfwords — position- and
 * bit-sensitive, ~38k iterations, and only ever run when a harness sets
 * autopilot.ini's vhash=1.  Zero cost otherwise. */
static void vhash_frame(void)
{
   size_t pitch;
   const uint16_t *pix = fe_host_last_frame(&pitch);
   unsigned w = g_last_w, h = g_last_h;
   uint32_t hash = 2166136261u;
   unsigned y;
   /* MEDIA ENGINE MODE: the core buffer is NEVER WRITTEN (capture mode skips
    * the render), so hashing it yields the same untouched buffer every frame.
    * This oracle silently reported "0 frames differ" for every ME-mode run in
    * the project -- 60 logs, all distinct=1 -- which is the answer it gives
    * for a renderer that draws nothing at all.  dump_frame_bmp() already
    * carries this switch; the oracle never got it.  Hash what was PRESENTED. */
   {
      const uint16_t *me = NULL;
      unsigned mp = 0;
      if (me_rend_presenting(&me, &mp))
      {
         pix   = me;
         pitch = mp * 2;   /* pitch is BYTES here */
         w = FE_GBA_WIDTH;
         h = FE_GBA_HEIGHT;
      }
   }
   if (!pix)
      return;
   for (y = 0; y < h; y++)
   {
      const uint16_t *row = (const uint16_t *)
                            ((const uint8_t *)pix + (size_t)y * pitch);
      unsigned x;
      if (g_vhash_legacy_rb)
         for (x = 0; x < w; x++)
         {
            unsigned px = row[x];
            hash ^= ((px & 0x001Fu) << 11) | (px & 0x07E0u) | (px >> 11);
            hash *= 16777619u;
         }
      else
      for (x = 0; x < w; x++)
      {
         hash ^= row[x];
         hash *= 16777619u;
      }
   }
   fe_evt("vhash f=%u h=%08x", fe_host_frame_count(), hash);
}

