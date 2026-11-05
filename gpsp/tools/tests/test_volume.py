#!/usr/bin/env python3
"""Settings > Volume (GitHub issue #31; docs/UI-OVERLAY.md section 15.5).

  1. CONFIG.INI: no key is 100 and a save writes no key, twice byte for
     byte; 0, 40, 100 round-trip; out-of-range and garbage read as 100; a
     value once written stays written (as 100 too); a corrupt in-memory value
     is repaired to 100.
  2. The channel volume (config_psp.h pcfg_audio_hw_volume): 100 is
     0x8000 exactly (what sceAudioOutputBlocking always got), 0 is 0, it
     never rises as the setting falls, and never exceeds 0x8000.
  3. The menu's step: +/-10, stopping at 0 and 100 (no wrap).
  4. main_psp.c's audio thread passes g_audio_hw_vol as the volume, the
     only sceAudioOutputBlocking call, and touches no sample with it (the
     out[] writes do not mention the volume); psp_audio_volume_apply sets it
     from pcfg_audio_hw_volume(g_pcfg.volume), right after the boot's
     config load (before audio_start) and from the Settings row.

Then mutants, each of which must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CFG = (ROOT / "psp/config_psp.c").read_text(encoding="utf-8")
CFGH = (ROOT / "psp/config_psp.h").read_text(encoding="utf-8")
MAIN = (ROOT / "psp/main_psp.c").read_text(encoding="utf-8")

TEST = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ctl_map.h"
#include "config_psp.h"
#include "fe_util.h"

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }
const char *vid_scale_name(int mode) { (void)mode; return "t"; }
const char *vid_filter_name(int mode) { (void)mode; return "t"; }

static const char *ini = "/tmp/@INI@";
static void write_file(const char *t)
{
   FILE *f = fopen(ini, "w");
   CHECK(f && fputs(t, f) >= 0);
   fclose(f);
}
static char *read_file(void)
{
   static char buf[16384];
   size_t n;
   FILE *f = fopen(ini, "r");
   CHECK(f);
   n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}
static int load(const char *text)
{
   write_file(text);
   memset(&g_pcfg, 0x5A, sizeof(g_pcfg));
   pcfg_load(ini);
   return g_pcfg.volume;
}

int main(void)
{
   static char first[16384];
   int v, prev;
   /* 1 */
   CHECK(load("scale = 1\n") == 100);
   pcfg_save();
   CHECK(strstr(read_file(), "volume") == NULL);
   snprintf(first, sizeof(first), "%s", read_file());
   CHECK(load(first) == 100);
   pcfg_save();
   CHECK(!strcmp(first, read_file()));
   g_pcfg.volume = 40;
   pcfg_save();
   CHECK(strstr(read_file(), "\nvolume = 40\n") != NULL);
   CHECK(load(read_file()) == 40);
   g_pcfg.volume = 0;
   pcfg_save();
   CHECK(load(read_file()) == 0);
   g_pcfg.volume = 100;
   pcfg_save();
   CHECK(strstr(read_file(), "\nvolume = 100\n") != NULL);
   CHECK(load(read_file()) == 100);
   CHECK(load("volume = 101\n") == 100);
   CHECK(load("volume = -1\n") == 100);
   CHECK(load("volume = loud\n") == 100);
   CHECK(load("volume = 70\n") == 70);
   load("scale = 1\n");
   g_pcfg.volume = 5000;
   pcfg_save();
   CHECK(g_pcfg.volume == 100 && strstr(read_file(), "volume") == NULL);
   remove(ini);

   /* 2 */
   CHECK(pcfg_audio_hw_volume(100) == 0x8000);
   CHECK(pcfg_audio_hw_volume(0) == 0);
   CHECK(pcfg_audio_hw_volume(1000) == 0x8000 && pcfg_audio_hw_volume(-5) == 0x8000);
   prev = 0;
   for (v = 0; v <= 100; v++)
   {
      int h = pcfg_audio_hw_volume(v);
      CHECK(h >= prev && h <= 0x8000);
      prev = h;
   }
   CHECK(pcfg_audio_hw_volume(50) == 0x4000);

   /* 3 */
   CHECK(pcfg_volume_step(100, +1) == 100 && pcfg_volume_step(100, -1) == 90);
   CHECK(pcfg_volume_step(0, -1) == 0 && pcfg_volume_step(0, +1) == 10);
   CHECK(pcfg_volume_step(95, +1) == 100 && pcfg_volume_step(5, -1) == 0);
   CHECK(pcfg_volume_step(777, -1) == 90);
   puts("volume: 100 by default and never written; round trips; 100 is the "
        "channel's 0x8000; steps of 10 stop at the ends");
   return 0;
}
'''

CC = ["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror"]


def run(cfg, cfgh, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-vol-") as folder:
        tag = Path(folder).name
        inc = Path(folder) / "inc"
        inc.mkdir()
        (inc / "config_psp.h").write_text(cfgh)
        c = Path(folder) / "config_psp.c"
        c.write_text(cfg)
        t = Path(folder) / "t.c"
        t.write_text(TEST.replace("@INI@", tag + ".ini"))
        b = Path(folder) / "t"
        subprocess.run(CC + ["-I" + str(inc), "-I" + str(ROOT / "psp"),
                             "-I" + str(ROOT / "frontend-common"), str(t),
                             str(c), str(ROOT / "psp/ctl_map.c"),
                             str(ROOT / "frontend-common/fe_util.c"),
                             "-o", str(b)], check=True)
        r = subprocess.run([str(b)], capture_output=True, text=True)
        if (r.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, r.stdout, r.stderr))
        return r


def audio_call(src):
    """4: the one output call, and the volume nowhere near a sample."""
    bad = []
    calls = re.findall(r"sceAudioOutputBlocking\(([^;]*)\);", src)
    if len(calls) != 1:
        bad.append("%d sceAudioOutputBlocking calls" % len(calls))
    elif "g_audio_hw_vol" not in calls[0]:
        bad.append("the output call does not take the setting")
    if "g_audio_hw_vol = pcfg_audio_hw_volume(g_pcfg.volume);" not in src:
        bad.append("the apply does not map the setting")
    load = src.find("pcfg_load_doc(CONFIG_INI")
    if (load < 0 or src.find("psp_audio_volume_apply();", load) < 0 or
            src.find("psp_audio_volume_apply();", load) >
            src.find("   audio_start();", load)):
        bad.append("the volume is not applied before audio_start")
    i = src.find("static int audio_thread(SceSize args, void *argp)")
    body = src[i:src.find("sceAudioOutputBlocking", i)]
    if re.search(r"out\[[^\]]*\]\s*=[^;]*(volume|hw_vol)", body):
        bad.append("a sample is scaled by the volume")
    return bad


def text(old, new, which):
    def mutate(srcs):
        s = dict(srcs)
        assert s[which].count(old) == 1, old
        s[which] = s[which].replace(old, new)
        return s
    return mutate


assert audio_call(MAIN) == [], audio_call(MAIN)
print("audio thread: one output call, volume = g_audio_hw_vol (applied at boot "
      "before audio_start), no sample scaled")
UI = (ROOT / "psp/ui_psp.c").read_text(encoding="utf-8")
assert re.search(r"g_pcfg\.volume = pcfg_volume_step\(g_pcfg\.volume, dir\);\s*"
                 r"psp_audio_volume_apply\(\);", UI), "the row does not apply"
print(run(CFG, CFGH, True).stdout.strip())

MUTANTS = (
    ("the default is 50", text("#define PCFG_VOLUME_DEFAULT 100", "#define PCFG_VOLUME_DEFAULT 50", "h"),
     'load("scale = 1'),
    ("the scale is off", text("   return PCFG_AUDIO_HW_MAX * v / 100;", "   return PCFG_AUDIO_HW_MAX * v / 128;", "h"),
     "pcfg_audio_hw_volume(50) == 0x4000"),
    ("unknown is silent", text("   if (v >= 100 || v < 0)\n", "   if (v >= 100)\n", "h"),
     "pcfg_audio_hw_volume(-5) == 0x8000"),
    ("the menu wraps", text("   return v < 0 ? 0 : v > 100 ? 100 : v;", "   return v < 0 ? 100 : v > 100 ? 0 : v;", "h"),
     "pcfg_volume_step(100, +1) == 100"),
    ("the key is always written",
     text("      if (g_pcfg.volume != PCFG_VOLUME_DEFAULT ||\n", "      if (1 ||\n", "c"),
     'strstr(read_file(), "volume") == NULL'),
    ("the key is never read",
     text('fe_ini_get_int(cfg_path, "volume", PCFG_VOLUME_DEFAULT)', "PCFG_VOLUME_DEFAULT", "c"),
     "== 40"),
    ("a corrupt value is not repaired",
     text("   PCFG_CHK(volume,               0, 100, PCFG_VOLUME_DEFAULT);\n", "", "c"),
     "g_pcfg.volume == 100"),
)
for label, mutate, expect in MUTANTS:
    s = mutate({"c": CFG, "h": CFGH})
    bad = run(s["c"], s["h"], False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant (%s): failed as it must" % label)

bad_main = MAIN.replace("sceAudioOutputBlocking(ch, g_audio_hw_vol, out)",
                        "sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, out)", 1)
assert bad_main != MAIN
assert audio_call(bad_main) == ["the output call does not take the setting"]
print("mutant (the audio thread ignores the setting): seen")
print("volume: %d mutants fail" % (len(MUTANTS) + 1))
