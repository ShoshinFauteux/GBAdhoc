/* test_state_delete.c -- save-state delete (docs/CONTROL-REMAP.md section 11).
 *
 * psp_state_delete_paths() is the ONLY thing that names the files a delete
 * removes, on the browser shelf and in game.  This checks that:
 *   1. for every console's slot-1 name and every slot it agrees with the
 *      path the save/load code uses (psp_state_path_for_slot) and with the
 *      two preview paths the save writes (psp_state_thumb_path,
 *      psp_state_shot_path);
 *   2. nothing else can come out of it: a ROM path, a .sav, a directory, a
 *      bare ".st0", a slot outside 1..10 and a buffer too small are refused
 *      with both outputs empty;
 *   3. a random fuzz of path strings only ever yields "<input minus its last
 *      digit><0-9>" and that plus ".thumb" / ".shot";
 *   4. on a real directory, deleting slot k removes exactly .st(k-1), its
 *      .thumb and its .shot and leaves the ROM, the .sav and every other
 *      slot alone;
 *   5. the .shot header the save writes is the one the page accepts, and
 *      the page refuses every other. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "state_slots.h"

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
   exit(1); } } while (0)

static int exists(const char *p)
{
   struct stat st;
   return stat(p, &st) == 0;
}

static void touch(const char *p)
{
   FILE *f = fopen(p, "w");
   CHECK(f != NULL);
   fputs("x", f);
   fclose(f);
}

/* What the PSP code does with the two names (ui_psp.c state_delete). */
static int do_delete(const char *slot1, unsigned slot)
{
   char st[PSP_FILE_PATH_CAP], th[PSP_STATE_THUMB_PATH_CAP];
   char sh[PSP_STATE_SHOT_PATH_CAP];
   if (psp_state_delete_paths(st, sizeof(st), th, sizeof(th), sh, sizeof(sh),
                              slot1, slot))
      return -1;
   if (remove(st) != 0 && exists(st))
      return -1;
   remove(th);
   remove(sh);
   return 0;
}

int main(void)
{
   static const char *roms[] = {
      "ms0:/PSP/GAME/gpsp-adhoc/roms/Pokemon Emerald.gba",
      "ms0:/PSP/GAME/gpsp-adhoc/roms/sub dir/Red.gb",
      "ms0:/PSP/GAME/gpsp-adhoc/roms/Crystal.gbc",
      "ms0:/roms.v2/Game.with.dots.gba",
   };
   char slot1[PSP_FILE_PATH_CAP], want[PSP_FILE_PATH_CAP];
   char st[PSP_FILE_PATH_CAP], th[PSP_STATE_THUMB_PATH_CAP];
   char wth[PSP_STATE_THUMB_PATH_CAP];
   char sh[PSP_STATE_SHOT_PATH_CAP], wsh[PSP_STATE_SHOT_PATH_CAP];
   unsigned r, c, slot;
   int i;

   /* 1. agreement with the save/load naming, every console, every slot */
   for (r = 0; r < sizeof(roms) / sizeof(roms[0]); r++)
      for (c = 0; c < 3; c++)
      {
         CHECK(psp_rom_path_suffix(slot1, sizeof(slot1), roms[r],
                  psp_state_slot1_suffix((fe_console_t)c)) == 0);
         for (slot = 1; slot <= PSP_STATE_SLOT_COUNT; slot++)
         {
            CHECK(psp_state_path_for_slot(want, sizeof(want), slot1,
                                          slot) == 0);
            CHECK(psp_state_thumb_path(wth, sizeof(wth), want) == 0);
            CHECK(psp_state_shot_path(wsh, sizeof(wsh), want) == 0);
            CHECK(psp_state_delete_paths(st, sizeof(st), th, sizeof(th),
                                         sh, sizeof(sh), slot1, slot) == 0);
            CHECK(strcmp(st, want) == 0);
            CHECK(strcmp(th, wth) == 0);
            CHECK(strcmp(sh, wsh) == 0);
            CHECK(strcmp(st, roms[r]) != 0);
         }
      }
   CHECK(psp_state_delete_paths(st, sizeof(st), th, sizeof(th), sh,
                                sizeof(sh), "/r/Ruby.st0", 3) == 0 &&
         !strcmp(st, "/r/Ruby.st2") && !strcmp(th, "/r/Ruby.st2.thumb") &&
         !strcmp(sh, "/r/Ruby.st2.shot"));
   CHECK(psp_state_delete_paths(st, sizeof(st), th, sizeof(th), sh,
                                sizeof(sh), "/r/EMERALD.ST0", 5) == 0 &&
         !strcmp(st, "/r/EMERALD.ST4"));
   /* the last slot: still one digit, still beside the others */
   CHECK(psp_state_delete_paths(st, sizeof(st), th, sizeof(th), sh,
                                sizeof(sh), "/r/Gold.gbc.st0", 10) == 0 &&
         !strcmp(st, "/r/Gold.gbc.st9") && !strcmp(th, "/r/Gold.gbc.st9.thumb") &&
         !strcmp(sh, "/r/Gold.gbc.st9.shot"));

   /* 2. refusals leave both outputs empty */
   {
      static const char *bad[] = {
         "/r/Ruby.gba", "/r/Ruby.sav", "/r/Ruby.gb.sav", "/r/Ruby.st1",
         "/r/Ruby.st0.thumb", "/r/.st0", "/r/", "", ".st0", "st0",
         "/r/Ruby.sT0x", "/r/Ruby", "ms0:.st0", "/r/Ruby.ss0",
      };
      for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++)
      {
         strcpy(st, "junk");
         strcpy(th, "junk");
         strcpy(sh, "junk");
         CHECK(psp_state_delete_paths(st, sizeof(st), th, sizeof(th), sh,
                                      sizeof(sh), bad[i], 1) == -1);
         CHECK(st[0] == '\0' && th[0] == '\0' && sh[0] == '\0');
      }
      for (slot = 0; slot < 16; slot += (slot == 0 ? PSP_STATE_SLOT_COUNT + 1 : 1))
      {
         strcpy(st, "junk");
         strcpy(sh, "junk");
         CHECK(psp_state_delete_paths(st, sizeof(st), th, sizeof(th), sh,
                                      sizeof(sh), "/r/Ruby.st0", slot) == -1);
         CHECK(st[0] == '\0' && th[0] == '\0' && sh[0] == '\0');
      }
      /* buffers too small: never a truncated name */
      CHECK(psp_state_delete_paths(st, 11, th, sizeof(th), sh, sizeof(sh),
                                   "/r/Ruby.st0", 2) == -1 && st[0] == '\0');
      CHECK(psp_state_delete_paths(st, 12, th, 17, sh, sizeof(sh),
                                   "/r/Ruby.st0", 2) == -1 &&
            st[0] == '\0' && th[0] == '\0' && sh[0] == '\0');
      CHECK(psp_state_delete_paths(st, 12, th, 18, sh, 16, "/r/Ruby.st0",
                                   2) == -1 &&
            st[0] == '\0' && th[0] == '\0' && sh[0] == '\0');
      CHECK(psp_state_delete_paths(st, 12, th, 18, sh, 17, "/r/Ruby.st0",
                                   2) == 0);
      CHECK(psp_state_delete_paths(NULL, 0, th, sizeof(th), sh, sizeof(sh),
                                   "/r/Ruby.st0", 2) == -1);
      CHECK(psp_state_delete_paths(st, sizeof(st), th, sizeof(th), NULL, 0,
                                   "/r/Ruby.st0", 2) == -1 &&
            st[0] == '\0' && th[0] == '\0');
   }

   /* 3. fuzz: whatever goes in, only a .stN / .stN.thumb comes out */
   srand(12345);
   for (i = 0; i < 200000; i++)
   {
      static const char alpha[] = "aZ09./: _-stST";
      char in[24];
      int n = rand() % 20, k;
      size_t ln;
      for (k = 0; k < n; k++)
         in[k] = alpha[rand() % (int)(sizeof(alpha) - 1)];
      in[n] = '\0';
      if (rand() % 3 == 0 && n < 15)
         strcat(in, rand() % 2 ? ".st0" : ".ST0");
      slot = (unsigned)(rand() % (PSP_STATE_SLOT_COUNT + 3));
      if (psp_state_delete_paths(st, sizeof(st), th, sizeof(th), sh,
                                 sizeof(sh), in, slot) != 0)
      {
         CHECK(st[0] == '\0' && th[0] == '\0' && sh[0] == '\0');
         continue;
      }
      ln = strlen(in);
      CHECK(slot >= 1 && slot <= PSP_STATE_SLOT_COUNT && strlen(st) == ln);
      CHECK(memcmp(st, in, ln - 1) == 0 && st[ln - 1] == (char)('0' + slot - 1));
      CHECK(st[ln - 4] == '.' && (st[ln - 3] | 0x20) == 's' &&
            (st[ln - 2] | 0x20) == 't');
      CHECK(strlen(th) == ln + 6 && memcmp(th, st, ln) == 0 &&
            strcmp(th + ln, ".thumb") == 0);
      CHECK(strlen(sh) == ln + 5 && memcmp(sh, st, ln) == 0 &&
            strcmp(sh + ln, ".shot") == 0);
   }

   /* 4. on disk: exactly three files go */
   {
      char dir[] = "/tmp/gbadhoc-del-XXXXXX";
      char p[256], base[256];
      static const char *keep[] = { "Ruby.gba", "Ruby.sav", "Ruby.st0",
         "Ruby.st0.thumb", "Ruby.st0.shot", "Ruby.st1", "Ruby.st3",
         "Ruby.st3.thumb", "Ruby.st3.shot", "Ruby.st4", "Ruby.gb.st2",
         "Ruby.gb.st2.shot", "Other.st2", "Other.st2.thumb",
         "Other.st2.shot", "Ruby.st5", "Ruby.st8", "Ruby.st8.thumb",
         "Ruby.st8.shot" };
      CHECK(mkdtemp(dir) != NULL);
      for (i = 0; i < (int)(sizeof(keep) / sizeof(keep[0])); i++)
      {
         snprintf(p, sizeof(p), "%s/%s", dir, keep[i]);
         touch(p);
      }
      snprintf(p, sizeof(p), "%s/Ruby.st2", dir);
      touch(p);
      snprintf(p, sizeof(p), "%s/Ruby.st2.thumb", dir);
      touch(p);
      snprintf(p, sizeof(p), "%s/Ruby.st2.shot", dir);
      touch(p);
      snprintf(base, sizeof(base), "%s/Ruby.st0", dir);
      CHECK(do_delete(base, 3) == 0);
      snprintf(p, sizeof(p), "%s/Ruby.st2", dir);
      CHECK(!exists(p));
      snprintf(p, sizeof(p), "%s/Ruby.st2.thumb", dir);
      CHECK(!exists(p));
      snprintf(p, sizeof(p), "%s/Ruby.st2.shot", dir);
      CHECK(!exists(p));
      for (i = 0; i < (int)(sizeof(keep) / sizeof(keep[0])); i++)
      {
         snprintf(p, sizeof(p), "%s/%s", dir, keep[i]);
         CHECK(exists(p));
      }
      /* an "old" state (no preview) deletes cleanly; a missing one is not
       * an error and removes nothing */
      CHECK(do_delete(base, 2) == 0);
      snprintf(p, sizeof(p), "%s/Ruby.st1", dir);
      CHECK(!exists(p));
      CHECK(do_delete(base, 3) == 0);
      /* slot 10 (.st9) goes the same way, and only it */
      snprintf(p, sizeof(p), "%s/Ruby.st9", dir);
      touch(p);
      snprintf(p, sizeof(p), "%s/Ruby.st9.thumb", dir);
      touch(p);
      snprintf(p, sizeof(p), "%s/Ruby.st9.shot", dir);
      touch(p);
      CHECK(do_delete(base, 10) == 0);
      snprintf(p, sizeof(p), "%s/Ruby.st9", dir);
      CHECK(!exists(p));
      snprintf(p, sizeof(p), "%s/Ruby.st9.thumb", dir);
      CHECK(!exists(p));
      snprintf(p, sizeof(p), "%s/Ruby.st9.shot", dir);
      CHECK(!exists(p));
      for (i = 0; i < (int)(sizeof(keep) / sizeof(keep[0])); i++)
      {
         snprintf(p, sizeof(p), "%s/%s", dir, keep[i]);
         CHECK(exists(p) || !strcmp(keep[i], "Ruby.st1"));
      }
      /* the ROM path itself is refused before anything is touched */
      snprintf(p, sizeof(p), "%s/Ruby.gba", dir);
      CHECK(do_delete(p, 1) == -1 && exists(p));
      for (i = 0; i < (int)(sizeof(keep) / sizeof(keep[0])); i++)
      {
         snprintf(p, sizeof(p), "%s/%s", dir, keep[i]);
         remove(p);
      }
      rmdir(dir);
   }

   /* 5. the .shot header: what the save writes, the page reads */
   {
      unsigned char hdr[16];
      unsigned w = 0, h = 0, k;
      psp_state_shot_header(hdr, 240, 160);
      CHECK(psp_state_shot_check(hdr, &w, &h) == 0 && w == 240 && h == 160);
      CHECK(!memcmp(hdr, "STH1\xF0\x00\xA0\x00\x00\x01", 10));
      for (k = 10; k < 16; k++)
         CHECK(hdr[k] == 0);
      psp_state_shot_header(hdr, 160, 144);
      CHECK(psp_state_shot_check(hdr, &w, &h) == 0 && w == 160 && h == 144);
      psp_state_shot_header(hdr, 241, 160);
      CHECK(psp_state_shot_check(hdr, &w, &h) == -1);
      psp_state_shot_header(hdr, 240, 161);
      CHECK(psp_state_shot_check(hdr, &w, &h) == -1);
      psp_state_shot_header(hdr, 0, 160);
      CHECK(psp_state_shot_check(hdr, &w, &h) == -1);
      psp_state_shot_header(hdr, 240, 160);
      hdr[8] = 64; hdr[9] = 0;           /* a .thumb's stride */
      CHECK(psp_state_shot_check(hdr, &w, &h) == -1);
      psp_state_shot_header(hdr, 240, 160);
      hdr[3] = '2';
      CHECK(psp_state_shot_check(hdr, &w, &h) == -1);
      /* a real .thumb header (64x42, stride 64) is never taken for a shot */
      memcpy(hdr, "STH1\x40\x00\x2A\x00\x40\x00\0\0\0\0\0\0", 16);
      CHECK(psp_state_shot_check(hdr, &w, &h) == -1);
   }

   printf("test_state_delete: OK (%d checks)\n", checks);
   return 0;
}
