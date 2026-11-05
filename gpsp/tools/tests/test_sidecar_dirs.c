/* test_sidecar_dirs.c -- psp/sidecar_dirs.c on the host (4.6.0-beta.4
 * folders): path mapping, mirrored subpaths, the per-console suffixes, the
 * state slots and their pictures, XMB launch-mode ROM paths, Share game
 * data's names, and the migration with failure injection (open, a full
 * stick, a corrupt or partial copy, close, rename, remove, mkdir): after
 * every failure the legacy files must be byte-for-byte what they were and
 * nothing may be left in the new folders.
 *
 *   gcc -std=gnu99 -DSC_HOST_TEST -Ipsp -Ifrontend-common \
 *       tools/tests/test_sidecar_dirs.c psp/sidecar_dirs.c \
 *       frontend-common/fe_util.c frontend-common/fe_share.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "sidecar_dirs.h"
#include "state_slots.h"
#include "rom_paths.h"
#include "fe_share.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, \
   __LINE__, #c); g_fail++; } } while (0)

/* ------------------------------------------------------- injecting fs */
typedef struct
{
   int fail_open_w;        /* the Nth write-open fails (1-based); 0 off */
   int open_w_count;
   long write_budget;      /* bytes the "stick" still takes; <0 = unlimited */
   int corrupt_w;          /* flip a byte in every write */
   int fail_close_w;
   int fail_rename;
   int fail_remove;
   int fail_mkdir;
   int fail_read_n;        /* the Nth read fails; 0 off */
   int read_count;
   int logs;
} inj_t;
static inj_t I;

typedef struct { FILE *f; int w; } hfile;

static void *t_open(void *u, const char *p, int w)
{
   hfile *h;
   FILE *f;
   (void)u;
   if (w && I.fail_open_w && ++I.open_w_count == I.fail_open_w)
      return NULL;
   f = fopen(p, w ? "wb" : "rb");
   if (!f)
      return NULL;
   h = (hfile *)malloc(sizeof(*h));
   h->f = f;
   h->w = w;
   return h;
}
static int t_read(void *u, void *hh, void *b, unsigned n)
{
   hfile *h = (hfile *)hh;
   (void)u;
   if (I.fail_read_n && ++I.read_count == I.fail_read_n)
      return -1;
   return (int)fread(b, 1, n, h->f);
}
static int t_write(void *u, void *hh, const void *b, unsigned n)
{
   hfile *h = (hfile *)hh;
   unsigned k = n;
   unsigned char tmp[4096];
   (void)u;
   if (I.write_budget >= 0 && (long)k > I.write_budget)
      k = (unsigned)I.write_budget;           /* the stick is full */
   if (I.write_budget >= 0)
      I.write_budget -= k;
   memcpy(tmp, b, k);
   if (I.corrupt_w && k)
      tmp[k / 2] ^= 0x5A;
   return (int)fwrite(tmp, 1, k, h->f);
}
static int t_close(void *u, void *hh)
{
   hfile *h = (hfile *)hh;
   int rc = fclose(h->f);
   int w = h->w;
   (void)u;
   free(h);
   if (w && I.fail_close_w)
      return -1;
   return rc == 0 ? 0 : -1;
}
static int t_exists(void *u, const char *p)
{
   struct stat st;
   (void)u;
   if (stat(p, &st) == 0)
      return 1;
   return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
}
static int t_rename(void *u, const char *a, const char *b)
{
   (void)u;
   if (I.fail_rename)
      return 0;            /* says ok, does nothing: the PSP's habit */
   return rename(a, b);
}
static int t_remove(void *u, const char *p)
{
   (void)u;
   if (I.fail_remove)
      return -1;
   return remove(p);
}
static int t_mkdir(void *u, const char *p)
{
   (void)u;
   if (I.fail_mkdir)
      return -1;
   return mkdir(p, 0777);
}
static void t_log(void *u, const char *l)
{
   (void)u;
   I.logs++;
   if (getenv("SC_VERBOSE"))
      printf("  log: %s\n", l);
}
static const sc_fs FS = { NULL, t_open, t_read, t_write, t_close, t_exists,
                          t_rename, t_remove, t_mkdir, t_log };

static void inj_reset(void)
{
   memset(&I, 0, sizeof(I));
   I.write_budget = -1;
}

/* ------------------------------------------------------------ helpers */
static char ROOT[256];       /* the app folder: <tmp>/GBAdhoc */
static char ROMS[300];

static void put(const char *path, const char *fill, size_t n)
{
   FILE *f = fopen(path, "wb");
   size_t i;
   if (!f) { printf("cannot write %s\n", path); exit(2); }
   for (i = 0; i < n; i++)
      fputc(fill[i % strlen(fill)], f);
   fclose(f);
}
static long fsize(const char *p)
{
   struct stat st;
   return stat(p, &st) == 0 ? (long)st.st_size : -1;
}
static int same_file(const char *a, const char *b)
{
   FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
   int ca, cb, same = fa && fb;
   while (same)
   {
      ca = fgetc(fa);
      cb = fgetc(fb);
      if (ca != cb) same = 0;
      if (ca == EOF) break;
   }
   if (fa) fclose(fa);
   if (fb) fclose(fb);
   return same;
}
static int exists(const char *p) { return fsize(p) >= 0; }
static void sh(const char *cmd) { if (system(cmd) != 0) { printf("cmd %s\n", cmd); exit(2); } }

/* Count every file under dir (recursive). */
static int count_files(const char *dir, const char *must_end)
{
   DIR *d = opendir(dir);
   struct dirent *e;
   int n = 0;
   if (!d)
      return 0;
   while ((e = readdir(d)))
   {
      char p[1024];
      struct stat st;
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
         continue;
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      if (stat(p, &st) != 0)
         continue;
      if (S_ISDIR(st.st_mode))
         n += count_files(p, must_end);
      else if (!must_end || (strlen(p) >= strlen(must_end) &&
               !strcmp(p + strlen(p) - strlen(must_end), must_end)))
         n++;
   }
   closedir(d);
   return n;
}

static void fresh(void)
{
   char cmd[600];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/roms/GBA/hacks' "
            "'%s/roms/GB'", ROOT, ROOT, ROOT);
   sh(cmd);
}

/* Paths the way main_psp.c makes them: legacy (beside the ROM) and new. */
static const char *save_sfx(int con)
{
   return con == FE_CONSOLE_GBA ? ".sav" : con == FE_CONSOLE_GBC ? ".gbc.sav"
                                                                 : ".gb.sav";
}
static void game_paths(const char *rom_path, int con, char *ls, char *lst,
                       char *ns, char *nst)
{
   char m[PSP_FILE_PATH_CAP];
   const char *rel = sc_rel_of(ROMS, rom_path);
   CHECK(psp_rom_path_suffix(ls, PSP_FILE_PATH_CAP, rom_path,
                             save_sfx(con)) == 0);
   CHECK(psp_rom_path_suffix(lst, PSP_FILE_PATH_CAP, rom_path,
                             psp_state_slot1_suffix((fe_console_t)con)) == 0);
   ns[0] = nst[0] = '\0';
   if (!rel)
      return;
   CHECK(sc_mirror(m, sizeof(m), ROOT, SC_SAVES_DIR, rel) == 0);
   CHECK(psp_rom_path_suffix(ns, PSP_FILE_PATH_CAP, m, save_sfx(con)) == 0);
   CHECK(sc_mirror(m, sizeof(m), ROOT, SC_STATES_DIR, rel) == 0);
   CHECK(psp_rom_path_suffix(nst, PSP_FILE_PATH_CAP, m,
                             psp_state_slot1_suffix((fe_console_t)con)) == 0);
}

/* ------------------------------------------------------------- tests */
static void test_paths(void)
{
   char a[PSP_FILE_PATH_CAP], b[PSP_FILE_PATH_CAP], c[PSP_FILE_PATH_CAP],
        d[PSP_FILE_PATH_CAP], th[PSP_STATE_THUMB_PATH_CAP],
        shp[PSP_STATE_SHOT_PATH_CAP], st[PSP_FILE_PATH_CAP];
   const char *R = "ms0:/PSP/GAME/GBAdhoc/roms";
   CHECK(!strcmp(sc_rel_of(R, "ms0:/PSP/GAME/GBAdhoc/roms/GBA/x.gba"),
                 "GBA/x.gba"));
   CHECK(!strcmp(sc_rel_of(R, "MS0:/psp/game/gbadhoc/ROMS/x.gba"), "x.gba"));
   CHECK(!strcmp(sc_rel_of("ms0:/PSP/GAME/GBAdhoc/roms/",
                           "ms0:/PSP/GAME/GBAdhoc/roms/x.gba"), "x.gba"));
   CHECK(sc_rel_of(R, "ms0:/PSP/GAME/GBAdhoc/roms2/x.gba") == NULL);
   CHECK(sc_rel_of(R, "ms0:/ROMS/x.gba") == NULL);
   CHECK(sc_rel_of(R, "ef0:/PSP/GAME/GBAdhoc/roms/x.gba") == NULL);
   CHECK(sc_rel_of(R, "ms0:/PSP/GAME/GBAdhoc/roms/../x.gba") == NULL);
   CHECK(sc_rel_of(R, "ms0:/PSP/GAME/GBAdhoc/roms/") == NULL);
   CHECK(sc_rel_of("ef0:/PSP/GAME/GBAdhoc/roms",
                   "ef0:/PSP/GAME/GBAdhoc/roms/GB/t.gb") != NULL);

   /* the mirror keeps the subpath and today's suffixes */
   strcpy(ROMS, R);
   strcpy(ROOT, "ms0:/PSP/GAME/GBAdhoc");
   game_paths("ms0:/PSP/GAME/GBAdhoc/roms/GBA/hacks/Game.gba",
              FE_CONSOLE_GBA, a, b, c, d);
   CHECK(!strcmp(a, "ms0:/PSP/GAME/GBAdhoc/roms/GBA/hacks/Game.sav"));
   CHECK(!strcmp(b, "ms0:/PSP/GAME/GBAdhoc/roms/GBA/hacks/Game.st0"));
   CHECK(!strcmp(c, "ms0:/PSP/GAME/GBAdhoc/saves/GBA/hacks/Game.sav"));
   CHECK(!strcmp(d, "ms0:/PSP/GAME/GBAdhoc/states/GBA/hacks/Game.st0"));
   game_paths("ms0:/PSP/GAME/GBAdhoc/roms/GB/Tetris.gb", FE_CONSOLE_GB,
              a, b, c, d);
   CHECK(!strcmp(c, "ms0:/PSP/GAME/GBAdhoc/saves/GB/Tetris.gb.sav"));
   CHECK(!strcmp(d, "ms0:/PSP/GAME/GBAdhoc/states/GB/Tetris.gb.st0"));
   game_paths("ms0:/PSP/GAME/GBAdhoc/roms/Zelda.gbc", FE_CONSOLE_GBC,
              a, b, c, d);
   CHECK(!strcmp(a, "ms0:/PSP/GAME/GBAdhoc/roms/Zelda.gbc.sav"));
   CHECK(!strcmp(c, "ms0:/PSP/GAME/GBAdhoc/saves/Zelda.gbc.sav"));
   CHECK(!strcmp(d, "ms0:/PSP/GAME/GBAdhoc/states/Zelda.gbc.st0"));
   /* a GBA and a GB game of one stem never share a file */
   game_paths("ms0:/PSP/GAME/GBAdhoc/roms/Same.gb", FE_CONSOLE_GB, a, b, c, d);
   CHECK(!strcmp(c, "ms0:/PSP/GAME/GBAdhoc/saves/Same.gb.sav"));
   game_paths("ms0:/PSP/GAME/GBAdhoc/roms/Same.gba", FE_CONSOLE_GBA,
              a, b, c, d);
   CHECK(!strcmp(c, "ms0:/PSP/GAME/GBAdhoc/saves/Same.sav"));
   /* slots and pictures follow the slot-1 path */
   game_paths("ms0:/PSP/GAME/GBAdhoc/roms/GB/T.gb", FE_CONSOLE_GB,
              a, b, c, d);
   CHECK(psp_state_path_for_slot(st, sizeof(st), d, 5) == 0 &&
         !strcmp(st, "ms0:/PSP/GAME/GBAdhoc/states/GB/T.gb.st4"));
   CHECK(psp_state_thumb_path(th, sizeof(th), st) == 0 &&
         !strcmp(th, "ms0:/PSP/GAME/GBAdhoc/states/GB/T.gb.st4.thumb"));
   CHECK(psp_state_shot_path(shp, sizeof(shp), st) == 0 &&
         !strcmp(shp, "ms0:/PSP/GAME/GBAdhoc/states/GB/T.gb.st4.shot"));
   /* delete derives only .stN files in the states folder */
   CHECK(psp_state_delete_paths(st, sizeof(st), th, sizeof(th), shp,
                                sizeof(shp), d, 3) == 0 &&
         !strcmp(st, "ms0:/PSP/GAME/GBAdhoc/states/GB/T.gb.st2") &&
         !strcmp(th, "ms0:/PSP/GAME/GBAdhoc/states/GB/T.gb.st2.thumb"));
   /* a path that does not fit is refused, never cut */
   {
      char big[PSP_ROM_REL_PATH_CAP + 8];
      memset(big, 'a', sizeof(big) - 1);
      big[sizeof(big) - 1] = '\0';
      CHECK(sc_mirror(a, 64, "ms0:/PSP/GAME/GBAdhoc", "saves", big) != 0 &&
            a[0] == '\0');
   }
   /* group members */
   {
      unsigned i, slot, n = 0;
      for (i = 0; sc_member(SC_GROUP_STATES, i, &slot); i++)
         n++;
      CHECK(n == 30);
      CHECK(sc_member(SC_GROUP_STATES, 29, &slot) && slot == 5);
      for (n = 0, i = 0; sc_member(SC_GROUP_SAVE, i, &slot); i++)
         n++;
      CHECK(n == 3);
   }
   printf("PASS paths: mirror, suffixes, slots, pictures, delete names\n");
}

/* XMB launch mode (695c923): argv[2] is relative to roms/ or an absolute
 * "dev:/" path; the sidecars are then the browser's for the same ROM. */
static void launch_rom_path(char *out, size_t cap, const char *arg)
{
   if (strchr(arg, ':'))
      snprintf(out, cap, "%s", arg);
   else
      CHECK(psp_rom_path_join(out, cap, ROMS, arg) == 0);
}

static void test_launch(void)
{
   char lp[PSP_FILE_PATH_CAP], bp[PSP_FILE_PATH_CAP];
   char la[PSP_FILE_PATH_CAP], lb[PSP_FILE_PATH_CAP], lc[PSP_FILE_PATH_CAP],
        ld[PSP_FILE_PATH_CAP];
   char ba[PSP_FILE_PATH_CAP], bb[PSP_FILE_PATH_CAP], bc[PSP_FILE_PATH_CAP],
        bd[PSP_FILE_PATH_CAP];
   static const char *const cases[][2] = {
      /* launch.ini rom =                         browser name */
      { "GBA/Kirby & The Amazing Mirror (USA).gba",
        "GBA/Kirby & The Amazing Mirror (USA).gba" },
      { "emerald.gba", "emerald.gba" },
      { "GB/hacks/Red.gb", "GB/hacks/Red.gb" },
      { "ef0:/PSP/GAME/GBAdhoc/roms/GBA/x.gba", "GBA/x.gba" },
   };
   unsigned i;
   strcpy(ROOT, "ef0:/PSP/GAME/GBAdhoc");
   strcpy(ROMS, "ef0:/PSP/GAME/GBAdhoc/roms");
   for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      int con = strstr(cases[i][1], ".gba") ? FE_CONSOLE_GBA : FE_CONSOLE_GB;
      launch_rom_path(lp, sizeof(lp), cases[i][0]);
      CHECK(psp_rom_path_join(bp, sizeof(bp), ROMS, cases[i][1]) == 0);
      game_paths(lp, con, la, lb, lc, ld);
      game_paths(bp, con, ba, bb, bc, bd);
      CHECK(lc[0] && !strcmp(lc, bc) && !strcmp(ld, bd));
      CHECK(!strncmp(lc, "ef0:/PSP/GAME/GBAdhoc/saves/", 28));
   }
   /* a ROM outside roms/ keeps its files beside it (no new path) */
   launch_rom_path(lp, sizeof(lp), "ms0:/ISO/gba/Outside.gba");
   game_paths(lp, FE_CONSOLE_GBA, la, lb, lc, ld);
   CHECK(lc[0] == '\0' && !strcmp(la, "ms0:/ISO/gba/Outside.sav") &&
         !strcmp(lb, "ms0:/ISO/gba/Outside.st0"));
   /* a ms0: path to roms/ when the app runs from ef0: is not this roms/ */
   launch_rom_path(lp, sizeof(lp), "ms0:/PSP/GAME/GBAdhoc/roms/GBA/x.gba");
   game_paths(lp, FE_CONSOLE_GBA, la, lb, lc, ld);
   CHECK(lc[0] == '\0');
   printf("PASS launch mode: relative, subfolder, absolute inside and outside "
          "roms/ resolve like the browser\n");
}

/* The old stick: a GBA game with everything beside it. */
typedef struct
{
   char rom[PSP_FILE_PATH_CAP];
   char ls[PSP_FILE_PATH_CAP], lst[PSP_FILE_PATH_CAP];
   char ns[PSP_FILE_PATH_CAP], nst[PSP_FILE_PATH_CAP];
   char legacy_files[12][PSP_FILE_PATH_CAP + 16];
   int n;
} fixture_t;

static void add_legacy(fixture_t *f, const char *path, const char *fill,
                       size_t n)
{
   put(path, fill, n);
   snprintf(f->legacy_files[f->n++], sizeof(f->legacy_files[0]), "%s", path);
}

static void old_stick(fixture_t *f, const char *rel, int con)
{
   char p[PSP_FILE_PATH_CAP + 16];
   memset(f, 0, sizeof(*f));
   fresh();
   CHECK(psp_rom_path_join(f->rom, sizeof(f->rom), ROMS, rel) == 0);
   put(f->rom, "ROM!", 4096);
   game_paths(f->rom, con, f->ls, f->lst, f->ns, f->nst);
   add_legacy(f, f->ls, "SRAMsramSRAM", 131072);
   snprintf(p, sizeof(p), "%s.bak", f->ls);
   add_legacy(f, p, "backup", 131072);
   add_legacy(f, f->lst, "state0", 70000);
   snprintf(p, sizeof(p), "%s.thumb", f->lst);
   add_legacy(f, p, "thumb", 5392);
   snprintf(p, sizeof(p), "%s.shot", f->lst);
   add_legacy(f, p, "shot", 76816);
   psp_state_path_for_slot(p, sizeof(p), f->lst, 3);
   add_legacy(f, p, "state2", 65000);
   /* a copy of what the stick held, to compare after a failure */
   {
      char cmd[700];
      snprintf(cmd, sizeof(cmd), "rm -rf '%s.orig' && cp -a '%s/roms' "
               "'%s.orig'", ROOT, ROOT, ROOT);
      sh(cmd);
   }
}

static int legacy_intact(const fixture_t *f)
{
   int i, ok = 1;
   size_t rl = strlen(ROOT);
   for (i = 0; i < f->n; i++)
   {
      char orig[PSP_FILE_PATH_CAP + 32];
      /* ROOT/roms/x -> ROOT.orig/x */
      snprintf(orig, sizeof(orig), "%s.orig%s", ROOT,
               f->legacy_files[i] + rl + strlen("/roms"));
      if (!same_file(f->legacy_files[i], orig))
      {
         printf("  legacy changed or gone: %s\n", f->legacy_files[i]);
         ok = 0;
      }
   }
   return ok;
}

static int new_is_empty(void)
{
   char d[400];
   int n;
   snprintf(d, sizeof(d), "%s/saves", ROOT);
   n = count_files(d, NULL);
   snprintf(d, sizeof(d), "%s/states", ROOT);
   n += count_files(d, NULL);
   return n == 0;
}

/* Both groups, as main_psp.c does when a game opens. */
static void open_game(fixture_t *f, const char **save, const char **state,
                      int *rs, int *rt)
{
   sc_report r;
   *rs = sc_settle(&FS, ROOT, SC_GROUP_SAVE, f->ls, f->ns, save, &r);
   *rt = sc_settle(&FS, ROOT, SC_GROUP_STATES, f->lst, f->nst, state, &r);
}

static void test_migrate_ok(void)
{
   fixture_t f;
   const char *s, *t;
   int rs, rt, i;
   char p[PSP_FILE_PATH_CAP + 16], q[PSP_FILE_PATH_CAP + 32];
   inj_reset();
   old_stick(&f, "GBA/hacks/Game.gba", FE_CONSOLE_GBA);
   open_game(&f, &s, &t, &rs, &rt);
   CHECK(rs == SC_MIG_MOVED && rt == SC_MIG_MOVED);
   CHECK(s == f.ns && t == f.nst);
   for (i = 0; i < f.n; i++)
   {
      /* every legacy file is gone and its copy is the original's bytes */
      const char *lf = f.legacy_files[i];
      const char *rel = lf + strlen(ROOT) + strlen("/roms/");
      int is_save = strstr(lf, ".sav") != NULL;
      snprintf(p, sizeof(p), "%s/%s/%s", ROOT, is_save ? "saves" : "states",
               rel);
      snprintf(q, sizeof(q), "%s.orig/%s", ROOT, rel);
      CHECK(!exists(lf));
      CHECK(same_file(p, q));
   }
   CHECK(exists(f.rom));                       /* ROMs are never moved */
   CHECK(count_files(ROOT, SC_MIG_SUFFIX) == 0);
   /* opened again: nothing to do, the new files are used */
   open_game(&f, &s, &t, &rs, &rt);
   CHECK(rs == SC_MIG_NONE && rt == SC_MIG_NONE && s == f.ns && t == f.nst);
   printf("PASS migrate: 6 files moved, verified, originals removed, ROM "
          "kept; second open is a no-op\n");
}

static void test_gb_suffixes_migrate(void)
{
   fixture_t f;
   const char *s, *t;
   int rs, rt;
   char p[PSP_FILE_PATH_CAP + 16];
   inj_reset();
   old_stick(&f, "GB/Tetris.gb", FE_CONSOLE_GB);
   CHECK(strstr(f.ls, "roms/GB/Tetris.gb.sav") != NULL);
   open_game(&f, &s, &t, &rs, &rt);
   CHECK(rs == SC_MIG_MOVED && rt == SC_MIG_MOVED);
   snprintf(p, sizeof(p), "%s/saves/GB/Tetris.gb.sav", ROOT);
   CHECK(exists(p));
   snprintf(p, sizeof(p), "%s/states/GB/Tetris.gb.st2", ROOT);
   CHECK(exists(p));
   snprintf(p, sizeof(p), "%s/states/GB/Tetris.gb.st0.thumb", ROOT);
   CHECK(exists(p));
   printf("PASS migrate GB: .gb.sav / .gb.stN / .thumb keep their names\n");
}

typedef void (*arm_fn)(void);
static void arm_open(void) { I.fail_open_w = 4; }
static void arm_full(void) { I.write_budget = 131072 + 1000; }
static void arm_corrupt(void) { I.corrupt_w = 1; }
static void arm_close(void) { I.fail_close_w = 1; }
static void arm_rename(void) { I.fail_rename = 1; }
static void arm_read(void) { I.fail_read_n = 3; }
static void arm_mkdir(void) { I.fail_mkdir = 1; }

static void test_failures(void)
{
   static const struct { const char *name; arm_fn arm; } arms[] = {
      { "write-open fails on the 4th file", arm_open },
      { "stick full partway (partial file)", arm_full },
      { "copy corrupted in flight", arm_corrupt },
      { "close of the copy fails", arm_close },
      { "rename does nothing", arm_rename },
      { "a read fails", arm_read },
      { "folders cannot be made", arm_mkdir },
   };
   unsigned a;
   for (a = 0; a < sizeof(arms) / sizeof(arms[0]); a++)
   {
      fixture_t f;
      const char *s, *t;
      int rs, rt;
      inj_reset();
      old_stick(&f, "GBA/hacks/Game.gba", FE_CONSOLE_GBA);
      arms[a].arm();
      open_game(&f, &s, &t, &rs, &rt);
      /* at least one group failed; whatever failed uses legacy; every
       * legacy file is unchanged; no .mig anywhere */
      CHECK(rs == SC_MIG_FAILED || rt == SC_MIG_FAILED);
      CHECK(rs != SC_MIG_FAILED || s == f.ls);
      CHECK(rt != SC_MIG_FAILED || t == f.lst);
      if (rs == SC_MIG_FAILED && rt == SC_MIG_FAILED)
         CHECK(legacy_intact(&f) && new_is_empty());
      else
      {
         /* one group got through (e.g. the stick filled in the second):
          * the other's legacy files are intact */
         int i;
         for (i = 0; i < f.n; i++)
         {
            int is_save = strstr(f.legacy_files[i], ".sav") != NULL;
            if ((is_save && rs == SC_MIG_FAILED) ||
                (!is_save && rt == SC_MIG_FAILED))
               CHECK(exists(f.legacy_files[i]));
         }
      }
      CHECK(count_files(ROOT, SC_MIG_SUFFIX) == 0);
      /* the stick recovers: the next open moves everything */
      inj_reset();
      open_game(&f, &s, &t, &rs, &rt);
      CHECK(rs != SC_MIG_FAILED && rt != SC_MIG_FAILED &&
            s == f.ns && t == f.nst);
      CHECK(!exists(f.ls) && !exists(f.lst));
      printf("PASS failure: %s -> legacy kept and used, retried later\n",
             arms[a].name);
   }
}

static void test_failure_mid_group(void)
{
   /* states: slot 1 and its pictures copy, slot 3 fails -> slot 1's copies
    * are taken back, nothing of the group is removed */
   fixture_t f;
   const char *t;
   sc_report r;
   int rt;
   char p[PSP_FILE_PATH_CAP + 16];
   inj_reset();
   old_stick(&f, "GBA/Game.gba", FE_CONSOLE_GBA);
   CHECK(sc_mkdirs_for(&FS, ROOT, f.nst) == 0);
   I.fail_open_w = 4;      /* st0, st0.thumb, st0.shot copy; st2 fails */
   rt = sc_settle(&FS, ROOT, SC_GROUP_STATES, f.lst, f.nst, &t, &r);
   CHECK(rt == SC_MIG_FAILED && t == f.lst && r.failed == 1);
   CHECK(legacy_intact(&f));
   CHECK(!exists(f.nst));
   snprintf(p, sizeof(p), "%s.thumb", f.nst);
   CHECK(!exists(p));
   printf("PASS failure: 4th file of a group -> its first 3 copies removed, "
          "group stays legacy\n");
}

static void test_remove_fails_then_dup(void)
{
   fixture_t f;
   const char *s, *t;
   int rs, rt;
   inj_reset();
   old_stick(&f, "GBA/Game.gba", FE_CONSOLE_GBA);
   I.fail_remove = 1;
   open_game(&f, &s, &t, &rs, &rt);
   CHECK(rs == SC_MIG_MOVED && rt == SC_MIG_MOVED && s == f.ns);
   CHECK(exists(f.ls) && exists(f.ns) && same_file(f.ls, f.ns));
   inj_reset();
   open_game(&f, &s, &t, &rs, &rt);       /* identical copies: old removed */
   CHECK(rs == SC_MIG_MOVED && !exists(f.ls) && exists(f.ns));
   CHECK(!exists(f.lst) && exists(f.nst));
   printf("PASS remove fails: new used, old left, removed on the next open "
          "once proven identical\n");
}

static void test_conflict(void)
{
   fixture_t f;
   const char *s, *t;
   int rs, rt;
   sc_report r;
   inj_reset();
   old_stick(&f, "GBA/Game.gba", FE_CONSOLE_GBA);
   CHECK(sc_mkdirs_for(&FS, ROOT, f.ns) == 0);
   put(f.ns, "NEWER", 131072);              /* e.g. saved by beta.4, then a
                                                beta.3 downgrade saved too */
   rs = sc_settle(&FS, ROOT, SC_GROUP_SAVE, f.ls, f.ns, &s, &r);
   CHECK(rs == SC_MIG_MOVED && s == f.ns && r.conflict == 1 && r.moved == 1);
   CHECK(exists(f.ls));                     /* the different one is kept */
   {
      char p[PSP_FILE_PATH_CAP + 8];
      snprintf(p, sizeof(p), "%s.bak", f.ls);
      CHECK(!exists(p));                    /* the .bak had no twin: moved */
   }
   (void)t; (void)rt;
   printf("PASS conflict: a different file in saves/ wins, the legacy one "
          "is never overwritten or removed\n");
}

static void test_interrupted_pass(void)
{
   /* power lost mid-copy: a .mig in the new folder, originals intact */
   fixture_t f;
   const char *s;
   char p[PSP_FILE_PATH_CAP + 8];
   int rs;
   inj_reset();
   old_stick(&f, "GBA/Game.gba", FE_CONSOLE_GBA);
   CHECK(sc_mkdirs_for(&FS, ROOT, f.ns) == 0);
   snprintf(p, sizeof(p), "%s" SC_MIG_SUFFIX, f.ns);
   put(p, "half", 1000);
   rs = sc_settle(&FS, ROOT, SC_GROUP_SAVE, f.ls, f.ns, &s, NULL);
   CHECK(rs == SC_MIG_MOVED && s == f.ns && !exists(p) && !exists(f.ls));
   printf("PASS interrupted pass: a stale .mig is replaced, not trusted\n");
}

static void test_recovery_files_travel(void)
{
   /* An interrupted whole-image write (save.old, no save) and an
    * interrupted share replace (save.bak only) move as they are, so
    * fe_host's recovery mends them in the new place. */
   fixture_t f;
   const char *s, *t;
   char p[PSP_FILE_PATH_CAP + 8], st1[PSP_FILE_PATH_CAP];
   int rs;
   inj_reset();
   fresh();
   memset(&f, 0, sizeof(f));
   psp_rom_path_join(f.rom, sizeof(f.rom), ROMS, "R.gba");
   put(f.rom, "ROM", 100);
   game_paths(f.rom, FE_CONSOLE_GBA, f.ls, f.lst, f.ns, f.nst);
   snprintf(p, sizeof(p), "%s.old", f.ls);
   put(p, "OLDIMAGE", 131072);
   psp_state_path_for_slot(st1, sizeof(st1), f.lst, 2);
   snprintf(p, sizeof(p), "%s.old", st1);
   put(p, "OLDSTATE", 5000);
   rs = sc_settle(&FS, ROOT, SC_GROUP_SAVE, f.ls, f.ns, &s, NULL);
   CHECK(rs == SC_MIG_MOVED && s == f.ns);
   snprintf(p, sizeof(p), "%s.old", f.ns);
   CHECK(fsize(p) == 131072 && !exists(f.ns));
   rs = sc_settle(&FS, ROOT, SC_GROUP_STATES, f.lst, f.nst, &t, NULL);
   psp_state_path_for_slot(st1, sizeof(st1), f.nst, 2);
   snprintf(p, sizeof(p), "%s.old", st1);
   CHECK(rs == SC_MIG_MOVED && fsize(p) == 5000);
   /* sc_pick sees a group that is only a .old */
   CHECK(sc_group_exists(&FS, SC_GROUP_STATES, f.nst));
   printf("PASS recovery files: .old / .bak travel with their group\n");
}

static void test_pick_and_first_run(void)
{
   fixture_t f;
   const char *o;
   char p[400];
   inj_reset();
   old_stick(&f, "GBA/Game.gba", FE_CONSOLE_GBA);
   /* legacy only -> legacy; new only -> new; both -> new; none -> new */
   CHECK(sc_pick(&FS, SC_GROUP_SAVE, f.ls, f.ns, &o) == 1 && o == f.ls);
   CHECK(sc_mkdirs_for(&FS, ROOT, f.ns) == 0);
   put(f.ns, "n", 10);
   CHECK(sc_pick(&FS, SC_GROUP_SAVE, f.ls, f.ns, &o) == 0 && o == f.ns);
   remove(f.ls);
   snprintf(p, sizeof(p), "%s.bak", f.ls);
   remove(p);
   remove(f.ns);
   CHECK(sc_pick(&FS, SC_GROUP_SAVE, f.ls, f.ns, &o) == 0 && o == f.ns);
   CHECK(sc_pick(&FS, SC_GROUP_SAVE, f.ls, NULL, &o) == 1 && o == f.ls);

   sh("true");
   snprintf(p, sizeof(p), "rm -rf '%s'", ROOT);
   sh(p);
   mkdir(ROOT, 0777);
   sc_first_run(&FS, ROOT, ROMS);
   snprintf(p, sizeof(p), "%s/GBA/.keep", ROMS);  CHECK(exists(p));
   snprintf(p, sizeof(p), "%s/GB/.keep", ROMS);   CHECK(exists(p));
   snprintf(p, sizeof(p), "%s/GBC/.keep", ROMS);  CHECK(exists(p));
   snprintf(p, sizeof(p), "%s/saves", ROOT);      CHECK(exists(p));
   snprintf(p, sizeof(p), "%s/states", ROOT);     CHECK(exists(p));
   I.logs = 0;
   sc_first_run(&FS, ROOT, ROMS);                 /* again: makes nothing */
   CHECK(I.logs == 0);
   /* a user who deleted roms/GB keeps it deleted?  No: it comes back,
    * empty -- documented; it is only a folder */
   printf("PASS pick (new > legacy > new) and first run (roms/GBA|GB|GBC "
          "+ .keep, saves/, states/)\n");
}

static void test_share_names(void)
{
   /* Share game data walks saves/ and states/ with the ROM's rel path:
    * the keys there must match the ROM exactly as beside it. */
   int con, kind, slot;
   size_t kl;
   CHECK(fe_share_classify("GBA/hacks/Game.sav", &con, &kind, &slot, &kl) &&
         con == 0 && kind == FE_SHARE_OBJ_SAVE &&
         fe_share_key_matches_rom("GBA/hacks/Game.sav", kl, 0,
                                  "GBA/hacks/Game.gba"));
   CHECK(fe_share_classify("GB/T.gb.st3.thumb", &con, &kind, &slot, &kl) &&
         con == 1 && kind == FE_SHARE_OBJ_THUMB && slot == 4 &&
         fe_share_key_matches_rom("GB/T.gb.st3.thumb", kl, 1, "GB/T.gb"));
   CHECK(!fe_share_classify("GBA/Game.sav" SC_MIG_SUFFIX, &con, &kind, &slot,
                            &kl));
   CHECK(!fe_share_classify("GBA/.keep", &con, &kind, &slot, &kl));
   printf("PASS share names: mirrored keys match their ROMs; .mig and .keep "
          "are not game data\n");
}

int main(void)
{
   char tmpl[] = "/tmp/sc_test_XXXXXX";
   char *t = mkdtemp(tmpl);
   if (!t)
      return 2;
   test_paths();
   test_launch();
   snprintf(ROOT, sizeof(ROOT), "%s/GBAdhoc", t);
   snprintf(ROMS, sizeof(ROMS), "%s/roms", ROOT);
   test_migrate_ok();
   test_gb_suffixes_migrate();
   test_failures();
   test_failure_mid_group();
   test_remove_fails_then_dup();
   test_conflict();
   test_interrupted_pass();
   test_recovery_files_travel();
   test_pick_and_first_run();
   test_share_names();
   {
      char cmd[300];
      snprintf(cmd, sizeof(cmd), "rm -rf '%s'", t);
      sh(cmd);
   }
   if (g_fail)
   {
      printf("FAIL sidecar_dirs: %d checks\n", g_fail);
      return 1;
   }
   printf("PASS sidecar_dirs: all\n");
   return 0;
}
