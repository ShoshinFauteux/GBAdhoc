#!/usr/bin/env python3
"""The pre-session .sav backup (PLAN HW-2 r3-B2), on the host with write faults.

backup_save and file_exists (fe/misc.b) are extracted unchanged from main_psp.c
(read_source) and run against real files in a temporary directory, with
fwrite / fread / fclose / rename able to fail on demand:

  1. a good copy: <save>.sav.bak equals the .sav, no .bak.tmp is left, and the
     .sav was landed first (sram flush + sync before the open);
  2. a write that fails part-way, a read error, a close that fails: the old
     .bak is untouched and no .tmp is left;
  3. a rename that fails: the complete copy is kept as .bak.tmp;
  4. a .sav shorter than 128 KiB is copied whole; no .sav: nothing written.
Then mutants (the old truncating writer, unchecked writes, no sync) are caught.
"""
from pathlib import Path
import re
import subprocess
import tempfile

import extract

ROOT = Path(__file__).resolve().parents[2]
SOURCE = extract.read_source(str(ROOT / "psp/main_psp.c"))


def production(source):
    fe = re.search(r"^static int file_exists\(const char \*path\)\n\{.*?^\}\n", source, re.S | re.M)
    bs = re.search(r"^static void backup_save\(const char \*save_path\)\n\{.*?^\}\n", source,
                   re.S | re.M)
    assert fe and bs, "backup_save / file_exists not found"
    return fe.group(0) + "\n" + bs.group(0)


STUB = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FE_SRAM_SIZE 131072
#define PSP_BACKUP_PATH_CAP 300
typedef struct { int x; } SceIoStat;
static int sceIoGetstat(const char *p, SceIoStat *st)
{ struct stat s; (void)st; return stat(p, &s) == 0 ? 0 : -1; }
static char order[256];
static int fail_write_at = -1, fail_read, fail_close, fail_rename, writes;
static int fe_host_sram_flush(int f) { (void)f; strcat(order, "flush "); return 0; }
static void fe_host_sram_sync(void) { strcat(order, "sync "); }
static void fe_evt(const char *f, ...) { (void)f; }

static size_t inj_fwrite(const void *b, size_t s, size_t n, FILE *f)
{
   if (writes++ == fail_write_at)
      return n / 2;
   return fwrite(b, s, n, f);
}
static size_t inj_fread(void *b, size_t s, size_t n, FILE *f)
{
   if (fail_read && strstr(order, "open") && writes >= fail_read)
   {
      return 0;
   }
   return fread(b, s, n, f);
}
static int inj_ferror(FILE *f) { return fail_read ? 1 : ferror(f); }
static int inj_fclose(FILE *f)
{
   int rc = fclose(f);
   return fail_close ? -1 : rc;
}
static int inj_rename(const char *a, const char *b)
{
   if (fail_rename)
      return -1;
   return rename(a, b);
}
static FILE *inj_fopen(const char *p, const char *m)
{
   if (!strcmp(m, "rb"))
      strcat(order, "open ");
   return fopen(p, m);
}
#define fwrite inj_fwrite
#define fread inj_fread
#define ferror inj_ferror
#define fclose(f) inj_fclose(f)
#define rename inj_rename
#define fopen inj_fopen

@FUNCTIONS@

#undef fopen
#undef fwrite
#undef fread
#undef fclose
#undef rename
#undef ferror

static char sav[200], bak[256], tmp[264], ref[256];
static void put(const char *p, int size, int seed)
{
   FILE *f = fopen(p, "wb");
   int i;
   for (i = 0; i < size; i++)
      fputc((i * 7 + seed) & 0xFF, f);
   fclose(f);
}
static int same(const char *a, const char *b)
{
   FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
   int ca, cb, ok = fa && fb;
   while (ok)
   {
      ca = fgetc(fa); cb = fgetc(fb);
      if (ca != cb) ok = 0;
      if (ca == EOF || cb == EOF) break;
   }
   if (fa) fclose(fa);
   if (fb) fclose(fb);
   return ok;
}
static int exists(const char *p) { struct stat s; return stat(p, &s) == 0; }
static void reset(void)
{
   order[0] = '\0';
   fail_write_at = -1; fail_read = fail_close = fail_rename = writes = 0;
   remove(tmp);
   put(sav, FE_SRAM_SIZE, 1);
   put(bak, FE_SRAM_SIZE, 2);           /* the previous good backup */
}

int main(int argc, char **argv)
{
   (void)argc;
   snprintf(sav, sizeof(sav), "%s/Ruby.sav", argv[1]);
   snprintf(bak, sizeof(bak), "%s.bak", sav);
   snprintf(tmp, sizeof(tmp), "%s.tmp", bak);
   snprintf(ref, sizeof(ref), "%s/ref.bin", argv[1]);
   put(ref, FE_SRAM_SIZE, 2);           /* what the old .bak holds */

   /* 1. good copy, the .sav landed first */
   reset();
   backup_save(sav);
   assert(same(sav, bak) && !exists(tmp));
   assert(!strncmp(order, "flush sync open", 15));

   /* 2. failures leave the old .bak alone */
   reset();
   put(tmp, 5, 9);                      /* a stale .tmp is replaced */
   fail_write_at = 3;
   backup_save(sav);
   assert(same(bak, ref) && !exists(tmp));
   reset();
   fail_close = 1;
   backup_save(sav);
   assert(same(bak, ref) && !exists(tmp));
   reset();
   fail_read = 2;
   backup_save(sav);
   assert(same(bak, ref) && !exists(tmp));

   /* 3. a failed rename keeps the complete copy */
   reset();
   fail_rename = 1;
   backup_save(sav);
   assert(exists(tmp) && same(sav, tmp));

   /* 4. a short .sav whole; no .sav, nothing */
   reset();
   put(sav, 8192 + 100, 3);
   backup_save(sav);
   assert(same(sav, bak) && !exists(tmp));
   remove(sav); remove(bak);
   backup_save(sav);
   assert(!exists(bak) && !exists(tmp));
   puts("backup_save: good copy, write/read/close/rename faults, short and missing .sav");
   return 0;
}
'''


def run(functions, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-backup-") as folder:
        cfile = Path(folder) / "b.c"
        binary = Path(folder) / "b"
        data = Path(folder) / "data"
        data.mkdir()
        cfile.write_text(STUB.replace("@FUNCTIONS@", functions))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Werror", "-Wno-unused-function",
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary), str(data)], capture_output=True, text=True)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


good = production(SOURCE)
print(run(good, True).stdout.strip())
MUTANTS = (
    ("the copy truncates the .bak itself", "out = fopen(tmp, \"wb\");", "out = fopen(bak, \"wb\");"),
    ("a short write is not checked", "      if (fwrite(buf, 1, n, out) != n)\n",
     "      if (fwrite(buf, 1, n, out) != n && 0)\n"),
    ("the close is not checked", "   if (fclose(out) != 0 && !bad)\n      bad = 3;\n",
     "   fclose(out);\n"),
    ("the .sav is not landed first", "   fe_host_sram_sync();           /* ... and no write of it in flight */\n",
     ""),
)
for what, old, new in MUTANTS:
    assert good.count(old) == 1, "mutant site gone: " + what
    run(good.replace(old, new), False)
    print("  mutant caught: " + what)
print("PASS: test_backup_save (%d mutants)" % len(MUTANTS))
