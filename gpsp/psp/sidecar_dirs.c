/* sidecar_dirs.c -- the saves/ and states/ folders and the one-time move of
 * a game's files out of roms/ (sidecar_dirs.h says what and why).
 *
 * No heap and no static buffers: every path is on the caller's stack, the
 * copy buffer is 2 KiB of stack, and the per-file status of a group (at most
 * 30 files) is a bit mask. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdint.h>

#include "sidecar_dirs.h"
#include "state_slots.h"
#include "fe_util.h"

#define SC_PATH  (PSP_FILE_PATH_CAP + 16)
#define SC_CHUNK 2048

/* ------------------------------------------------------------------ paths */

const char *sc_rel_of(const char *rom_dir, const char *rom_path)
{
   size_t n;
   if (!rom_dir || !rom_path)
      return NULL;
   n = strlen(rom_dir);
   while (n && rom_dir[n - 1] == '/')
      n--;
   if (!n || strncasecmp(rom_path, rom_dir, n) != 0 || rom_path[n] != '/')
      return NULL;
   rom_path += n;
   while (*rom_path == '/')
      rom_path++;
   /* a rel that climbs out of roms/ is not inside it */
   if (!*rom_path || strstr(rom_path, "../") || !strcmp(rom_path, ".."))
      return NULL;
   return rom_path;
}

int sc_mirror(char *out, size_t cap, const char *base, const char *dir,
              const char *rel)
{
   int n;
   if (!out || !cap)
      return -1;
   out[0] = '\0';
   if (!base || !dir || !rel || !rel[0])
      return -1;
   n = snprintf(out, cap, "%s/%s/%s", base, dir, rel);
   if (n < 0 || (size_t)n >= cap)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

static void sc_logf(const sc_fs *fs, const char *fmt, ...)
{
   char line[2 * SC_PATH + 64];
   va_list ap;
   if (!fs->log)
      return;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   fs->log(fs->user, line);
}

int sc_mkdirs_for(const sc_fs *fs, const char *base, const char *file_path)
{
   char dir[SC_PATH];
   size_t bl, i;
   char *slash;
   if (!fs || !base || !file_path)
      return -1;
   bl = strlen(base);
   if (snprintf(dir, sizeof(dir), "%s", file_path) >= (int)sizeof(dir))
      return -1;
   slash = strrchr(dir, '/');
   if (!slash || (size_t)(slash - dir) < bl ||
       strncasecmp(dir, base, bl) != 0)
      return -1;
   *slash = '\0';
   if (fs->exists(fs->user, dir) == 1)
      return 0;
   /* base, then each folder under it, in order */
   for (i = bl; ; i++)
   {
      if (dir[i] == '/' || dir[i] == '\0')
      {
         char c = dir[i];
         dir[i] = '\0';
         if (i > 0 && fs->exists(fs->user, dir) != 1)
            fs->mkdir(fs->user, dir);
         dir[i] = c;
         if (!c)
            break;
      }
   }
   return fs->exists(fs->user, dir) == 1 ? 0 : -1;
}

void sc_first_run(const sc_fs *fs, const char *base, const char *rom_dir)
{
   static const char *const con[3] = { "GBA", "GB", "GBC" };
   char p[SC_PATH];
   int i;
   if (!fs || !base || !rom_dir)
      return;
   if (fs->exists(fs->user, rom_dir) != 1)
      fs->mkdir(fs->user, rom_dir);
   for (i = 0; i < 3; i++)
   {
      if (snprintf(p, sizeof(p), "%s/%s", rom_dir, con[i]) >= (int)sizeof(p))
         continue;
      if (fs->exists(fs->user, p) == 1)
         continue;
      fs->mkdir(fs->user, p);
      if (snprintf(p, sizeof(p), "%s/%s/.keep", rom_dir, con[i]) <
          (int)sizeof(p))
      {
         void *h = fs->open(fs->user, p, 1);
         if (h)
            fs->close(fs->user, h);
      }
      sc_logf(fs, "folders_made dir=%s/%s", rom_dir, con[i]);
   }
   if (snprintf(p, sizeof(p), "%s/" SC_SAVES_DIR, base) < (int)sizeof(p) &&
       fs->exists(fs->user, p) != 1)
      fs->mkdir(fs->user, p);
   if (snprintf(p, sizeof(p), "%s/" SC_STATES_DIR, base) < (int)sizeof(p) &&
       fs->exists(fs->user, p) != 1)
      fs->mkdir(fs->user, p);
}

/* ----------------------------------------------------------------- groups */

static const char *const k_save_sfx[] = { "", ".bak", ".old" };
static const char *const k_state_sfx[] = { "", ".old", ".thumb",
                                           ".thumb.old", ".shot",
                                           ".shot.old" };
#define N_SAVE_SFX  (sizeof(k_save_sfx) / sizeof(k_save_sfx[0]))
#define N_STATE_SFX (sizeof(k_state_sfx) / sizeof(k_state_sfx[0]))
#define SC_GROUP_MAX (PSP_STATE_SLOT_COUNT * N_STATE_SFX)

const char *sc_member(int group, unsigned i, unsigned *slot)
{
   if (group == SC_GROUP_SAVE)
   {
      if (slot)
         *slot = 0;
      return i < N_SAVE_SFX ? k_save_sfx[i] : NULL;
   }
   if (i >= SC_GROUP_MAX)
      return NULL;
   if (slot)
      *slot = i / N_STATE_SFX + 1;
   return k_state_sfx[i % N_STATE_SFX];
}

/* The path of member i of the group at `path`.  0 = fits. */
static int sc_member_path(char *out, size_t cap, int group, const char *path,
                          unsigned i)
{
   unsigned slot;
   const char *sfx = sc_member(group, i, &slot);
   char base[SC_PATH];
   int n;
   out[0] = '\0';
   if (!sfx)
      return -1;
   if (group == SC_GROUP_SAVE)
      n = snprintf(out, cap, "%s%s", path, sfx);
   else
   {
      if (psp_state_path_for_slot(base, sizeof(base), path, slot) != 0)
         return -1;
      n = snprintf(out, cap, "%s%s", base, sfx);
   }
   return n > 0 && (size_t)n < cap ? 0 : -1;
}

int sc_group_exists(const sc_fs *fs, int group, const char *path)
{
   char p[SC_PATH];
   unsigned i;
   for (i = 0; sc_member(group, i, NULL); i++)
      if (sc_member_path(p, sizeof(p), group, path, i) == 0 &&
          fs->exists(fs->user, p) == 1)
         return 1;
   return 0;
}

int sc_pick(const sc_fs *fs, int group, const char *legacy_path,
            const char *new_path, const char **out)
{
   int legacy = !new_path || (!sc_group_exists(fs, group, new_path) &&
                              legacy_path &&
                              sc_group_exists(fs, group, legacy_path));
   if (out)
      *out = legacy ? legacy_path : new_path;
   return legacy;
}

/* ------------------------------------------------------- copy and verify */

/* Size and CRC32 of a whole file.  0 = read to the end. */
static int sc_digest(const sc_fs *fs, const char *path, uint32_t *size,
                     uint32_t *crc)
{
   unsigned char buf[SC_CHUNK];
   void *h = fs->open(fs->user, path, 0);
   uint32_t s = 0, c = 0;
   int n, ok = 1;
   if (!h)
      return -1;
   for (;;)
   {
      n = fs->read(fs->user, h, buf, sizeof(buf));
      if (n < 0)
      {
         ok = 0;
         break;
      }
      if (n == 0)
         break;
      c = fe_crc32(c, buf, (size_t)n);
      s += (uint32_t)n;
   }
   if (fs->close(fs->user, h) != 0)
      ok = 0;
   *size = s;
   *crc = c;
   return ok ? 0 : -1;
}

static int sc_rename(const sc_fs *fs, const char *from, const char *to)
{
   (void)fs->rename(fs->user, from, to);
   /* PSP and PPSSPP disagree about rename's return value (fe_host.c's
    * state_file_rename): the names decide. */
   return fs->exists(fs->user, from) == 0 && fs->exists(fs->user, to) == 1
      ? 0 : -1;
}

/* Copy `from` to `to` via to.mig, verified.  0 = `to` is a checked copy.
 * Leaves no .mig behind; on failure `to` does not exist (it did not
 * before: the caller checked). */
static int sc_copy(const sc_fs *fs, const char *from, const char *to)
{
   char tmp[SC_PATH + 8];
   unsigned char buf[SC_CHUNK];
   uint32_t s0 = 0, c0 = 0, s1, c1;
   void *in, *out;
   int n, ok = 1;
   if (snprintf(tmp, sizeof(tmp), "%s" SC_MIG_SUFFIX, to) >= (int)sizeof(tmp))
      return -1;
   fs->remove(fs->user, tmp);             /* an interrupted earlier pass */
   in = fs->open(fs->user, from, 0);
   if (!in)
      return -1;
   out = fs->open(fs->user, tmp, 1);
   if (!out)
   {
      fs->close(fs->user, in);
      return -1;
   }
   for (;;)
   {
      n = fs->read(fs->user, in, buf, sizeof(buf));
      if (n < 0)
      {
         ok = 0;
         break;
      }
      if (n == 0)
         break;
      if (fs->write(fs->user, out, buf, (unsigned)n) != n)
      {
         ok = 0;                           /* a full stick, a write error */
         break;
      }
      c0 = fe_crc32(c0, buf, (size_t)n);
      s0 += (uint32_t)n;
   }
   fs->close(fs->user, in);
   if (fs->close(fs->user, out) != 0)
      ok = 0;
   /* the copy as the stick has it, against what was read from the source */
   if (ok && (sc_digest(fs, tmp, &s1, &c1) != 0 || s1 != s0 || c1 != c0))
      ok = 0;
   /* ...and the source again: it must not have changed under us */
   if (ok && (sc_digest(fs, from, &s1, &c1) != 0 || s1 != s0 || c1 != c0))
      ok = 0;
   if (ok && sc_rename(fs, tmp, to) != 0)
      ok = 0;
   if (ok && (sc_digest(fs, to, &s1, &c1) != 0 || s1 != s0 || c1 != c0))
   {
      fs->remove(fs->user, to);
      ok = 0;
   }
   if (!ok)
      fs->remove(fs->user, tmp);
   return ok ? 0 : -1;
}

/* 1 = same size and bytes, 0 = different, -1 = could not tell. */
static int sc_same(const sc_fs *fs, const char *a, const char *b)
{
   uint32_t sa, ca, sb, cb;
   if (sc_digest(fs, a, &sa, &ca) != 0 || sc_digest(fs, b, &sb, &cb) != 0)
      return -1;
   return sa == sb && ca == cb;
}

int sc_migrate(const sc_fs *fs, int group, const char *legacy_path,
               const char *new_path, sc_report *rep)
{
   char op[SC_PATH], np[SC_PATH];
   uint32_t copied = 0, drop = 0;          /* per member: made / remove old */
   sc_report r;
   unsigned i;
   int any = 0, bad = 0;
   memset(&r, 0, sizeof(r));
   if (!fs || !legacy_path || !new_path || !strcasecmp(legacy_path, new_path))
   {
      if (rep)
         *rep = r;
      return SC_MIG_NONE;
   }
   for (i = 0; !bad && sc_member(group, i, NULL); i++)
   {
      int e;
      if (sc_member_path(op, sizeof(op), group, legacy_path, i) != 0 ||
          sc_member_path(np, sizeof(np), group, new_path, i) != 0)
         continue;
      e = fs->exists(fs->user, op);
      if (e == 0)
         continue;
      any = 1;
      if (e < 0)
      {
         bad = 1;
         r.failed++;
         break;
      }
      e = fs->exists(fs->user, np);
      if (e < 0)
      {
         bad = 1;
         r.failed++;
         break;
      }
      if (e == 1)
      {
         int s = sc_same(fs, op, np);
         if (s == 1)
         {
            drop |= 1u << i;               /* an earlier pass's copy */
            r.dup++;
         }
         else if (s == 0)
         {
            r.conflict++;
            sc_logf(fs, "folders_conflict kept=%s used=%s", op, np);
         }
         else
         {
            bad = 1;
            r.failed++;
         }
         continue;
      }
      if (sc_copy(fs, op, np) != 0)
      {
         bad = 1;
         r.failed++;
         sc_logf(fs, "folders_copy_failed from=%s to=%s", op, np);
         break;
      }
      copied |= 1u << i;
      drop |= 1u << i;
      r.moved++;
   }
   if (!any)
   {
      if (rep)
         *rep = r;
      return SC_MIG_NONE;
   }
   if (bad)
   {
      /* Take back this pass's copies: every original is still in place. */
      for (i = 0; sc_member(group, i, NULL); i++)
         if ((copied & (1u << i)) &&
             sc_member_path(np, sizeof(np), group, new_path, i) == 0)
            fs->remove(fs->user, np);
      r.moved = 0;
      sc_logf(fs, "folders_migrate_failed group=%s keep=%s",
              group == SC_GROUP_SAVE ? "save" : "states", legacy_path);
      if (rep)
         *rep = r;
      return SC_MIG_FAILED;
   }
   /* Every file arrived and checked: now, and only now, the originals go. */
   for (i = 0; sc_member(group, i, NULL); i++)
   {
      if (!(drop & (1u << i)) ||
          sc_member_path(op, sizeof(op), group, legacy_path, i) != 0)
         continue;
      if (fs->remove(fs->user, op) != 0 && fs->exists(fs->user, op) == 1)
      {
         r.left++;                          /* retried as a dup next time */
         sc_logf(fs, "folders_remove_failed file=%s", op);
      }
      else
      {
         sc_member_path(np, sizeof(np), group, new_path, i);
         sc_logf(fs, "folders_moved from=%s to=%s", op, np);
      }
   }
   if (rep)
      *rep = r;
   return SC_MIG_MOVED;
}

int sc_settle(const sc_fs *fs, const char *base, int group,
              const char *legacy_path, const char *new_path,
              const char **use, sc_report *rep)
{
   int rc;
   if (rep)
      memset(rep, 0, sizeof(*rep));
   *use = legacy_path;
   if (!new_path || !new_path[0])
      return SC_MIG_NONE;
   if (sc_mkdirs_for(fs, base, new_path) != 0)
   {
      sc_logf(fs, "folders_mkdir_failed file=%s", new_path);
      return SC_MIG_FAILED;
   }
   rc = sc_migrate(fs, group, legacy_path, new_path, rep);
   if (rc != SC_MIG_FAILED)
      *use = new_path;
   return rc;
}

/* ------------------------------------------------------------- PSP sceIo */
#ifndef SC_HOST_TEST
#include <pspiofilemgr.h>
#include "fe_evt.h"

static void *psp_open(void *u, const char *p, int w)
{
   SceUID fd = sceIoOpen(p, w ? (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC)
                              : PSP_O_RDONLY, 0777);
   (void)u;
   return fd < 0 ? NULL : (void *)(intptr_t)(fd + 1);
}
static int psp_read(void *u, void *h, void *b, unsigned n)
{
   (void)u;
   return sceIoRead((SceUID)((intptr_t)h - 1), b, n);
}
static int psp_write(void *u, void *h, const void *b, unsigned n)
{
   (void)u;
   return sceIoWrite((SceUID)((intptr_t)h - 1), b, n);
}
static int psp_close(void *u, void *h)
{
   (void)u;
   return sceIoClose((SceUID)((intptr_t)h - 1)) < 0 ? -1 : 0;
}
static int psp_exists(void *u, const char *p)
{
   SceIoStat st;
   (void)u;
   return sceIoGetstat(p, &st) >= 0 ? 1 : 0;
}
static int psp_rename(void *u, const char *a, const char *b)
{
   (void)u;
   return sceIoRename(a, b) < 0 ? -1 : 0;
}
static int psp_remove(void *u, const char *p)
{
   (void)u;
   return sceIoRemove(p) < 0 ? -1 : 0;
}
static int psp_mkdir(void *u, const char *p)
{
   (void)u;
   return sceIoMkdir(p, 0777) < 0 ? -1 : 0;
}
static void psp_log(void *u, const char *line)
{
   (void)u;
   fe_evt("%s", line);
   (void)line;
}

const sc_fs sc_fs_psp = { NULL, psp_open, psp_read, psp_write, psp_close,
                          psp_exists, psp_rename, psp_remove, psp_mkdir,
                          psp_log };
#endif
