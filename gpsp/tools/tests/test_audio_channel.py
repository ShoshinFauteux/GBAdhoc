#!/usr/bin/env python3
"""The audio worker's sceAudio channel across a stop, run on the host.

Extracts, unchanged, audio_thread, audio_start and audio_stop from
psp/main_psp.c and runs them on a real host thread against a driver stub
with the PSP's 8 output channels.  Checked:

  1. A worker that returns releases its channel itself, once: a join that
     times out after it has returned does not release it again.
  2. A worker stuck in sceAudioOutputBlocking is terminated by audio_stop
     with its channel still reserved; audio_stop releases it.  Nine
     terminate-restarts in a row (one more than there are channels) each
     find a channel, so the game is never left silent.

Then mutants of the production text, each of which must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from test_psp_runtime import function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "psp/main_psp.c").read_text(encoding="utf-8")


def production(source):
    parts = []
    for pattern, what in ((r"^static volatile int audio_ch\b.*$", "audio_ch"),
                          (r"^#define AUDIO_JOIN_US .*$", "AUDIO_JOIN_US")):
        m = re.search(pattern, source, re.M)
        assert m, what + " not found"
        parts.append(m.group(0))
    for name in ("audio_thread", "audio_start", "audio_stop"):
        parts.append(function(source, name))
    return "\n\n".join(parts)


STUB = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
typedef unsigned SceSize;
typedef unsigned SceUInt;
typedef int SceUID;
#define OUT_CHUNK 16
#define OUT_RATE 44100
#define RING_FRAMES 256
#define RING_MASK (RING_FRAMES - 1)
#define AUDIO_FADE_STEP (65536 / 64)
#define PSP_AUDIO_NEXT_CHANNEL (-1)
#define PSP_AUDIO_FORMAT_STEREO 0
#define PSP_AUDIO_VOLUME_MAX 0x8000
#define THREAD_ATTR_USER 0x80000000
#define WAIT_TIMEOUT (-0x7FFDFE58)
static int16_t audio_ring[RING_FRAMES * 2];
static volatile unsigned ring_w, ring_r, g_audio_step;
static volatile unsigned g_audio_starved, g_audio_underruns;
static volatile int audio_running, g_audio_soft, audio_output_error;
static unsigned in_rate = 32768;
static SceUID audio_thid = -1;

/* The driver: 8 output channels, as on the PSP. */
static pthread_mutex_t drv = PTHREAD_MUTEX_INITIALIZER;
static int in_use[8], reserved, released, bad_releases;
static volatile int playing;      /* the worker reached its output */
static volatile int stuck;        /* ...and never comes back from it */
static volatile int late;         /* the join times out although it ended */
static int sceAudioChReserve(int c, int n, int f)
{
   int i;
   (void)c; (void)n; (void)f;
   pthread_mutex_lock(&drv);
   for (i = 0; i < 8; i++)
      if (!in_use[i])
      {
         in_use[i] = 1;
         reserved++;
         pthread_mutex_unlock(&drv);
         return i;
      }
   pthread_mutex_unlock(&drv);
   return -0x7FD9FFFE;            /* no free channel */
}
static int sceAudioChRelease(int ch)
{
   int rc = 0;
   pthread_mutex_lock(&drv);
   if (ch < 0 || ch >= 8 || !in_use[ch])
   {
      bad_releases++;
      rc = -1;
   }
   else
   {
      in_use[ch] = 0;
      released++;
   }
   pthread_mutex_unlock(&drv);
   return rc;
}
static int sceAudioOutputBlocking(int ch, int vol, void *buf)
{
   (void)ch; (void)vol; (void)buf;
   playing = 1;
   while (stuck)
      usleep(1000);               /* a cancellation point: terminate */
   usleep(1000);
   return 0;
}
static int channels_in_use(void)
{
   int i, n = 0;
   pthread_mutex_lock(&drv);
   for (i = 0; i < 8; i++)
      n += in_use[i];
   pthread_mutex_unlock(&drv);
   return n;
}

/* The kernel's threads, as host threads. */
static int (*entry)(SceSize, void *);
static pthread_t worker;
static int joined;
static void *trampoline(void *p)
{
   (void)p;
   entry(0, NULL);
   return NULL;
}
static SceUID sceKernelCreateThread(const char *name, int (*fn)(SceSize, void *),
                                    int prio, int stack, unsigned attr, void *opt)
{
   (void)name; (void)prio; (void)stack; (void)attr; (void)opt;
   entry = fn;
   return 7;
}
static int sceKernelStartThread(SceUID id, SceSize n, void *a)
{
   (void)id; (void)n; (void)a;
   joined = 0;
   playing = 0;
   return pthread_create(&worker, NULL, trampoline, NULL) ? -1 : 0;
}
static int sceKernelWaitThreadEnd(SceUID id, SceUInt *tmo)
{
   (void)id; (void)tmo;
   if (stuck)
      return WAIT_TIMEOUT;
   pthread_join(worker, NULL);
   joined = 1;
   return late ? WAIT_TIMEOUT : 0;
}
static int sceKernelTerminateDeleteThread(SceUID id)
{
   (void)id;
   if (!joined)
   {
      pthread_cancel(worker);
      pthread_join(worker, NULL);
      joined = 1;
   }
   return 0;
}
static int sceKernelDeleteThread(SceUID id) { (void)id; return 0; }

@FUNCTIONS@

static void wait_playing(void)
{
   int i;
   for (i = 0; i < 2000 && !playing && audio_running; i++)
      usleep(1000);
}

int main(void)
{
   int i, rc;

   /* 1. A worker that returns gives its channel back itself, once. */
   audio_start();
   wait_playing();
   assert(playing && channels_in_use() == 1);
   rc = audio_stop();
   assert(rc == 0 && channels_in_use() == 0);
   assert(released == 1 && bad_releases == 0);

   /* ...and a join that runs out just as it returns terminates a thread
    * that has already given the channel back: it is not released again. */
   audio_start();
   wait_playing();
   late = 1;
   rc = audio_stop();
   late = 0;
   assert(rc < 0 && channels_in_use() == 0);
   assert(released == 2 && bad_releases == 0);

   /* 2. A worker stuck in its output is terminated holding its channel;
    *    the stop releases it, so the restart finds one.  One more round
    *    than there are channels. */
   for (i = 0; i < 9; i++)
   {
      stuck = 1;
      audio_start();
      wait_playing();
      assert(audio_output_error == 0 && audio_running);
      assert(channels_in_use() == 1);
      rc = audio_stop();
      stuck = 0;
      assert(rc < 0);
      assert(channels_in_use() == 0);
   }
   assert(reserved == 11 && released == 11 && bad_releases == 0);

   puts("audio channel: released on return, not twice on a late join, "
        "released after a terminate, 9 terminate-restarts pass");
   return 0;
}
'''


def run(functions, expect_pass):
    with tempfile.TemporaryDirectory(prefix="gbadhoc-audioch-") as folder:
        cfile = Path(folder) / "ach.c"
        binary = Path(folder) / "ach"
        cfile.write_text(STUB.replace("@FUNCTIONS@", functions))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-pthread",
                        str(cfile), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                timeout=60)
        if (result.returncode == 0) != expect_pass:
            raise AssertionError("expect_pass=%s\n%s%s" % (
                expect_pass, result.stdout, result.stderr))
        return result


print(run(production(SOURCE), True).stdout.strip())

MUTANTS = (
    ("a terminated worker's channel is not released",
     "            sceAudioChRelease(audio_ch);\n", "",
     "channels_in_use() == 0"),
    ("the worker never says which channel it holds",
     "   audio_ch = ch;\n", "",
     "channels_in_use() == 0"),
    ("the worker releases its channel without clearing it",
     "   audio_ch = -1;\n   sceAudioChRelease(ch);",
     "   sceAudioChRelease(ch);",
     "bad_releases == 0"),
)
for label, old, new, expect in MUTANTS:
    bad_src = SOURCE.replace(old, new, 1)
    assert bad_src != SOURCE, label
    bad = run(production(bad_src), False)
    assert expect in bad.stderr, (label, bad.stderr)
    print("mutant (%s): failed as it must" % label)
