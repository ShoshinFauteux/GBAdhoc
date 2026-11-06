#!/usr/bin/env python3
"""The dynarec SMC tables' heap block refuses instead of abort()ing
(cpu_threaded.c dynarec_heap_tables_init / _release, libretro.c
retro_init / retro_load_game; critique-4 should-fix 4).

cpu_threaded.c's two functions, as written, are compiled against a malloc
that fails on demand (with the shipped XS_HEAP switches), and checked:

  1. malloc refused: init returns -1, leaves every table pointer NULL and
     holds no block (no leak), and a later init that is allowed succeeds.
  2. success: one block of the size the tables need, every table pointer
     inside it, zeroed; a second init is a no-op (no second malloc).
  3. release frees that block and NULLs every pointer; init/release cycles
     balance (malloc count == free count), the instance-B session pattern.

libretro.c, as text:
  4. retro_init records the result both ways (xs_heap_failed = ... != 0),
     before init_gamepak_buffer's malloc-until-failure loop;
  5. retro_load_game refuses (return false) on xs_heap_failed before it
     reads any variable or allocates.

Then mutants, each of which must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from test_psp_runtime import function

ROOT = Path(__file__).resolve().parents[2]
CPU = (ROOT / "cpu_threaded.c").read_text(encoding="utf-8")
LIBRETRO = (ROOT / "libretro/libretro.c").read_text(encoding="utf-8")

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
#define SMC_SAME_SKIP
#define SMC_PARTIAL_SINGLE
#define SMC_SAME_ADAPT
#define SMC_RETIRE_LOCAL
#define SMC_RETIRE_WINDOW
#define SMC_LOCAL_SHIFT 6

static int fail_next, n_malloc, n_free;
static void *live;
static size_t live_sz;
static void *t_malloc(size_t n)
{
   void *p;
   if (fail_next) { fail_next = 0; return NULL; }
   p = malloc(n);
   n_malloc++;
   live = p; live_sz = n;
   return p;
}
static void t_free(void *p)
{
   if (!p) return;
   n_free++;
   if (p == live) live = NULL;
   free(p);
}
#define malloc t_malloc
#define free t_free

static u16 *xs_val;
static u8  *xs_ok, *xs_bad;
static u16 *xs_min;
static u8 (*xs_rec)[0x10000 / 16];
static u16 *smc_back_iw, *smc_back_ew;
static u8 *xs_heap_blk;

@FUNCS@

#undef malloc
#undef free

static int all_null(void)
{
   return !xs_val && !xs_ok && !xs_bad && !xs_min && !xs_rec &&
          !smc_back_iw && !smc_back_ew;
}

static int inside(const void *p)
{
   return (const u8 *)p >= (const u8 *)live &&
          (const u8 *)p < (const u8 *)live + live_sz;
}

int main(void)
{
   size_t i;
   int cycle;
   /* 1 */
   fail_next = 1;
   assert(dynarec_heap_tables_init() == -1);
   assert(all_null());
   assert(n_malloc == 0 && n_free == 0);
   /* 2 */
   assert(dynarec_heap_tables_init() == 0);
   assert(n_malloc == 1 && live);
   assert(live_sz == 0x8000 + 2 * (0x8000 / 16) + 0x8000 / 2 +
                     2 * (0x10000 / 16) +
                     (0x8000 >> 6) * 2 + (0x40000 >> 6) * 2);
   assert(inside(xs_val) && inside(xs_ok) && inside(xs_bad) &&
          inside(xs_min) && inside(xs_rec) && inside(smc_back_iw) &&
          inside(smc_back_ew));
   for (i = 0; i < live_sz; i++)
      assert(((u8 *)live)[i] == 0);
   assert(dynarec_heap_tables_init() == 0);
   assert(n_malloc == 1);
   /* 3 */
   dynarec_heap_tables_release();
   assert(all_null() && n_free == 1 && !live);
   for (cycle = 0; cycle < 8; cycle++)
   {
      fail_next = (cycle % 3) == 1;
      if (dynarec_heap_tables_init() == 0)
         dynarec_heap_tables_release();
      else
         assert(all_null());
   }
   dynarec_heap_tables_release();      /* a second release is harmless */
   assert(n_malloc == n_free && !live);
   printf("xs_heap_refuse: malloc=%d free=%d OK\n", n_malloc, n_free);
   return 0;
}
'''


def build_and_run(cpu_src, libretro_src):
    funcs = function(cpu_src, "dynarec_heap_tables_release") + "\n" + \
        function(cpu_src, "dynarec_heap_tables_init")
    # 4: retro_init records the result both ways, before the ROM loop.
    m = re.search(r"xs_heap_failed\s*=\s*dynarec_heap_tables_init\(\)\s*!=\s*0;",
                  libretro_src)
    if not m:
        return "retro_init does not record dynarec_heap_tables_init's result"
    r = libretro_src.rfind("void retro_init(void)", 0, m.start())
    end = libretro_src.find("\n}\n", r) if r >= 0 else -1
    loop = libretro_src.find("init_gamepak_buffer();", m.end())
    if r < 0 or not (m.start() < loop < end):
        return "the tables are not taken inside retro_init before the ROM loop"
    # 5: retro_load_game refuses first.
    i = libretro_src.find("bool retro_load_game(")
    if i < 0:
        return "retro_load_game not found"
    body = libretro_src[i:]
    refuse = re.search(r"if\s*\(xs_heap_failed\)\s*return false;", body)
    first_use = min([j for j in (body.find("check_variables("),
                                 body.find("malloc(")) if j >= 0])
    if not refuse or refuse.start() > first_use:
        return "retro_load_game does not refuse on xs_heap_failed first"
    src = HARNESS.replace("@FUNCS@", funcs)
    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / "t.c"
        exe = Path(d) / "t"
        c.write_text(src, encoding="utf-8")
        r = subprocess.run(["gcc", "-std=gnu99", "-O1", "-g", "-Wall",
                            "-fsanitize=address,undefined",
                            str(c), "-o", str(exe)],
                           capture_output=True, text=True)
        if r.returncode:
            return "compile failed:\n" + r.stderr
        r = subprocess.run([str(exe)], capture_output=True, text=True,
                           env={"ASAN_OPTIONS": "detect_leaks=1",
                                "UBSAN_OPTIONS": "halt_on_error=1"})
        if r.returncode:
            return "run failed:\n" + r.stdout + r.stderr
        print(r.stdout.strip())
    return None


def main():
    err = build_and_run(CPU, LIBRETRO)
    assert err is None, err
    mutants = [
        ("a refusal abort()s again (the pre-round-4 behaviour)",
         CPU.replace("    return -1;\n  }\n  xs_heap_blk = blk;",
                     "    abort();\n  }\n  xs_heap_blk = blk;"),
         LIBRETRO),
        ("refusal returns success",
         CPU.replace("    return -1;\n  }\n  xs_heap_blk = blk;",
                     "    return 0;\n  }\n  xs_heap_blk = blk;"),
         LIBRETRO),
        ("release forgets a pointer",
         CPU.replace("  smc_back_iw = smc_back_ew = NULL;\n", "  smc_back_iw = NULL;\n", 1),
         LIBRETRO),
        ("release leaks the block",
         CPU.replace("  free(xs_heap_blk);\n  xs_heap_blk = NULL;",
                     "  xs_heap_blk = NULL;"),
         LIBRETRO),
        ("init is not idempotent",
         CPU.replace("  if (xs_heap_blk)\n    return 0;\n", "", 1),
         LIBRETRO),
        ("load ignores the refusal", CPU,
         re.sub(r"if\s*\(xs_heap_failed\)\s*return false;", ";", LIBRETRO)),
        ("init result not recorded", CPU,
         LIBRETRO.replace("xs_heap_failed = dynarec_heap_tables_init() != 0;",
                          "(void)dynarec_heap_tables_init();")),
    ]
    for name, cpu, lr in mutants:
        assert cpu != CPU or lr != LIBRETRO, "mutant did not apply: " + name
        assert build_and_run(cpu, lr) is not None, "mutant survived: " + name
        print("mutant killed:", name)
    print("test_xs_heap_refuse: OK")


if __name__ == "__main__":
    main()
