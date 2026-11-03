#!/usr/bin/env python3
"""Execute selected production PSP lifecycle functions with deterministic stubs.

The frontend translation unit depends on the entire PSP application. Extract
complete functions unchanged, so these tests exercise its current decisions,
not a hand-maintained copy. The ME driver is compiled in full with SDK stubs.
No PSP device, firmware, filesystem save, or real thread is touched.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise AssertionError("production function not found: " + name)
    # These selected functions have no brace literals; comments may have them.
    start = match.start()
    body = match.end() - 1
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, body):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:token.end()]
    raise AssertionError("unclosed function: " + name)


FRONT = r'''
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
typedef unsigned SceSize;
typedef unsigned SceUInt;
#define IO_WAIT_US 250000
#define FE_EVT_ONLY(x) ((void)(x))
#define fe_evt(...) ((void)0)
static struct { int log_thread, sram_thread, standby; } g_pcfg;
static int g_io_run, g_io_sema, log_calls, save_calls, free_calls;
static int loaded, healthy, idle, sync_ok, sync_calls;
static unsigned clock_us;
static void *g_mer_work, *g_mer_cap[2], *g_mer_stage[2], *g_mer_desc;
static int g_mer_vmem, g_me_rend, g_mer_heap_pending_drain;
static void *g_mer_sh_vram, *g_mer_sh_oam, *g_mer_sh_pal;
static int g_mer_sh_on, g_mer_ready, g_mer_pending, g_mer_last_presented;
static int unlock_calls;
static int g_pwr_had_host, g_pwr_slept, ring_r, ring_w, blank_calls;
static void me_standby_step(const char *s) { (void)s; }
static void wake_snapshot(void) {}
static int me_rend_active(void) { return g_me_rend; }
static int retained, shutdowns;
static void me_host_shutdown(void) { loaded=healthy=0; retained=0; shutdowns++; }
static void vid_blank_all(void) { blank_calls++; }
#define MER_VM_UNLOCK() (unlock_calls++)
static int sceKernelWaitSema(int id, int n, unsigned *timeout)
{ (void)id; (void)n; (void)timeout; g_io_run=0; return 0; }
static void fe_evt_service(void) { log_calls++; }
static void fe_host_sram_service_io(void) { save_calls++; }
static int me_host_up(void) { return healthy; }
static int me_host_loaded(void) { return loaded; }
static int me_host_retained(void) { return retained; }
static int g_pwr_hold, pwr_releases, lock_drops;
static void pwr_release(void) { g_pwr_hold--; pwr_releases++; }
static void play_lock_drop(const char *who) { (void)who; lock_drops++; }
static int me_host_idle(void) { return idle; }
static int me_host_sync(unsigned timeout)
{ (void)timeout; sync_calls++; return healthy && idle && sync_ok ? 0 : -1; }
static unsigned sceKernelGetSystemTimeLow(void) { return clock_us += 1000; }
static void me_rend_teardown(const char *why) { (void)why; g_me_rend=0; }
static void counted_free(void *p) { if(p) free_calls++; }
#define free counted_free
@FUNCTIONS@
#undef free
static int baseline;
static void check(int condition, const char *name)
{ printf("%s: %s\n", name, condition ? "PASS" : "FAIL"); if(!baseline) assert(condition); }
int main(int argc, char **argv)
{
   int l,s;
   (void)argv; baseline = argc > 1;
   for(l=0;l<2;l++) for(s=0;s<2;s++)
   {
      char label[80];
      g_pcfg.log_thread=l; g_pcfg.sram_thread=s;
      g_io_run=1; log_calls=save_calls=0;
      io_thread(0,NULL);
      snprintf(label,sizeof(label),"writer ownership log=%d save=%d",l,s);
      check(log_calls==2*l && save_calls==2*s,label);
   }
   /* A watchdog failure is not a stopped module. No buffer, including a
    * descriptor that a delayed command may still read, may be freed. */
   loaded=1; healthy=0; idle=0; sync_ok=0;
   g_mer_work=(void*)1; g_mer_desc=(void*)2;
   g_mer_cap[0]=(void*)3; g_mer_cap[1]=(void*)4;
   g_mer_stage[0]=(void*)5; g_mer_stage[1]=(void*)6;
   free_calls=0; g_mer_heap_pending_drain=0;
   me_rend_suspend();
   check(free_calls==0 && g_mer_heap_pending_drain,
         "watchdog-dead in-flight renderer retains all buffers");
   /* Even an idle dead engine may retain dirty work-cache lines. */
   idle=1; free_calls=0; g_mer_work=(void*)1;
   check(mer_work_heap_free()<0 && free_calls==0,
         "watchdog-dead idle renderer retains cached work area");
   loaded=healthy=idle=sync_ok=1; free_calls=sync_calls=0; g_mer_work=(void*)1;
   check(mer_work_heap_free()==0 && free_calls==1 && sync_calls==1,
         "healthy idle renderer syncs before freeing work");
   loaded=healthy=0; free_calls=sync_calls=0; g_mer_work=(void*)1;
   check(mer_work_heap_free()==0 && free_calls==1 && sync_calls==0,
         "stopped renderer frees work without posting a command");
   g_mer_vmem=1;g_mer_work=(void*)1;loaded=1;healthy=idle=0;
   check(me_rend_release_vmem()<0 && unlock_calls==0 && g_mer_work,
         "busy dead ME retains volatile pool ownership");
   healthy=idle=1;sync_ok=0;
   check(me_rend_release_vmem()<0 && unlock_calls==0 && g_mer_work,
         "cache flush timeout retains volatile pool ownership");
   sync_ok=1;
   check(me_rend_release_vmem()==1 && unlock_calls==1 && !g_mer_work,
         "completed cache flush permits volatile pool release");
   /* Issue 28: a sleep from the game list.  The retain already emptied the
    * ME's cache, and a job posted from the callback would race the next
    * game taking the module up. */
   g_mer_vmem=1;g_mer_work=(void*)1;loaded=retained=1;healthy=idle=sync_ok=1;
   sync_calls=0;unlock_calls=0;
   check(me_rend_release_vmem()==1 && unlock_calls==1 && !sync_calls,
         "a retained idle module gives the pool back with no job posted");
   /* Every exit of the process is one call: a module kept for a game that
    * never came is stopped there, one in use is left to the exit's teardown. */
   loaded=retained=1; healthy=1; shutdowns=0; g_pwr_hold=2; pwr_releases=0;
   pwr_release_all();
   check(shutdowns==1 && !retained && !g_pwr_hold && pwr_releases==1 && lock_drops==1,
         "leaving the process stops a retained module");
   loaded=healthy=1; retained=0; shutdowns=0;
   pwr_release_all();
   check(!shutdowns, "leaving does not touch a module in use");
   g_mer_vmem=1; loaded=1; healthy=idle=sync_ok=0;
   g_pcfg.standby=1; g_pwr_slept=0; ring_r=2; ring_w=7; blank_calls=0;
   me_standby_down();
   check(!g_pwr_slept && g_mer_vmem && ring_r==2 && !blank_calls,
         "deferred standby release preserves unslept retained-pool state");
   healthy=idle=sync_ok=1;
   me_standby_down();
   check(g_pwr_slept && !g_mer_vmem && ring_r==7 && blank_calls==1,
         "successful standby release marks sleep and clears presentation");
   return 0;
}
'''

SDK = r'''
#ifndef PSP_TEST_SDK_H
#define PSP_TEST_SDK_H
typedef int SceUID;
typedef unsigned SceSize;
void sceKernelDcacheWritebackInvalidateRange(const void *, unsigned);
void sceKernelDcacheWritebackRange(const void *, unsigned);
int sceKernelStartModule(SceUID, unsigned, void *, int *, void *);
int sceKernelStopModule(SceUID, unsigned, void *, int *, void *);
int sceKernelUnloadModule(SceUID);
int sceKernelDelayThread(unsigned);
unsigned sceKernelGetSystemTimeLow(void);
SceUID kuKernelLoadModule(const char *, int, void *);
#endif
'''

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <malloc.h>
#include "psp/me/me_mbox.h"
#undef ME_UNCACHED
#define ME_UNCACHED(p) ((void *)(p))
static int baseline, allocations, frees, stop_calls, stop_fail;
static int complete_job, handshake, answer_sync, load_calls, beats;
static void *allocated[8];
static void *tracked_align(size_t alignment, size_t size)
{
   void *p=memalign(alignment,size); assert(allocations<8);
   allocated[allocations++]=p; return p;
}
static void tracked_free(void *p)
{
   int i;
   if(!p)return;
   for(i=0;i<allocations;i++) if(allocated[i]==p) allocated[i]=NULL;
   frees++; free(p);
}
#define memalign tracked_align
#define free tracked_free
#include "psp/me_host.c"
#undef free
#undef memalign
void fe_evt(const char *fmt, ...) { (void)fmt; }
void sceKernelDcacheWritebackInvalidateRange(const void *p,unsigned n) {(void)p;(void)n;}
void sceKernelDcacheWritebackRange(const void *p,unsigned n) {(void)p;(void)n;}
SceUID kuKernelLoadModule(const char *p,int f,void *o) {(void)p;(void)f;(void)o;load_calls++;return 7;}
int sceKernelStartModule(SceUID id,unsigned n,void *a,int *s,void *o)
{ (void)id;(void)n;(void)a;(void)o;if(s)*s=0;return 7; }
int sceKernelStopModule(SceUID id,unsigned n,void *a,int *s,void *o)
{ (void)id;(void)n;(void)a;(void)o;stop_calls++;if(s)*s=stop_fail?-1:0;return stop_fail?-1:0; }
int sceKernelUnloadModule(SceUID id) { (void)id; return 0; }
int sceKernelDelayThread(unsigned us)
{
   (void)us;
   if(handshake) {g_mbox_storage.magic=ME_MBOX_MAGIC;g_mbox_storage.version=ME_MBOX_VERSION;}
   if(beats) g_mbox_storage.heartbeat++;
   if(complete_job && g_mb)
   {
      g_mb->result=host_sum32(allocated[allocations-2],g_mb->arg2);
      g_mb->done_seq=g_mb->cmd_seq;
   }
   return 0;
}
unsigned sceKernelGetSystemTimeLow(void)
{
   static unsigned t;
   /* the ME's side of ME_CMD_SYNC, which is spun on, not slept on */
   if(answer_sync && g_mb && g_mbox_storage.cmd==ME_CMD_SYNC)
      g_mbox_storage.done_seq=g_mbox_storage.cmd_seq;
   return t+=1000;
}
static void check(int condition, const char *name)
{ printf("%s: %s\n", name, condition ? "PASS" : "FAIL"); if(!baseline) assert(condition); }
/* Issue 28: the module outlives the game, and is stopped only by leaving. */
static void retain_cases(void)
{
   int stops=stop_calls;
   g_me_handed_back=0;      /* the cases above stopped their module */
   handshake=1;complete_job=0;
   check(me_host_init(".")==0,"retain: module up");
   check(!me_host_handed_back() && !me_host_retained(),"retain: nothing retained yet");
   /* a render still running: the core may not be freed under it */
   g_mb->cmd=ME_CMD_RENDER;g_mb->cmd_seq=g_mb->cmd_seq+1;
   check(me_host_retain(50000u)==0 && !me_host_retained(),
         "retain refuses an engine that never finishes its job");
   g_mb->done_seq=g_mb->cmd_seq;
   answer_sync=0;
   check(me_host_retain(50000u)==0 && !me_host_retained(),
         "retain refuses an engine that does not flush its cache");
   g_mb->done_seq=g_mb->cmd_seq;
   answer_sync=1;
   check(me_host_retain(50000u)==1 && me_host_retained() && me_host_loaded(),
         "retain keeps a healthy idle engine, cache flushed");
   check(stop_calls==stops,"retain does not stop the module");
   check(g_mb->cmd==ME_CMD_SYNC,"retain flushes the ME cache");
   /* the next game: no load, no cleared mailbox, a moving heartbeat */
   {
      int loads=load_calls;
      unsigned seq=g_mb->cmd_seq;
      g_me_beat_stale=29;beats=1;
      check(me_host_init(".")==0 && load_calls==loads && g_mb->cmd_seq==seq &&
            !me_host_retained(),"the next game takes the module up, no reload");
      check(me_host_watchdog_frame()==0,"taking up resets the watchdog window");
   }
   /* quit again: retain; the process leaves: a real stop */
   check(me_host_retain(50000u)==1,"second retain");
   me_host_shutdown();
   check(stop_calls==stops+1 && !me_host_loaded() && !me_host_retained() &&
         me_host_handed_back(),"leaving stops a retained module");
   /* a sleep that left the ME silent: CPU rendering, and the module stays */
   g_me_handed_back=0;
   check(me_host_init(".")==0,"retain: module up again");
   check(me_host_retain(50000u)==1,"retained for the silent case");
   beats=0;
   check(me_host_init(".")<0 && me_host_loaded() && me_host_was_dead() &&
         !me_host_up(),"a silent retained engine is declared dead, not trusted");
   check(me_host_retain(50000u)==0,"a dead engine cannot be retained");
   g_me_dead=0;answer_sync=0;
}
int main(int argc, char **argv)
{
   int i;
   (void)argv;baseline=argc>1;
   g_mb=&g_mbox_storage;g_me_modid=7;g_me_dead=0;
   g_mb->cmd_seq=g_mb->done_seq=0;
   check(me_host_bench(64,1)<0,"benchmark times out when job never acknowledges");
   check(frees==0,"timed-out benchmark retains buffers for late ME writes");
   stop_fail=1;
   me_host_shutdown();
   check(frees==0 && g_me_modid==7 && g_me_dead,
         "failed stop retains module ownership and benchmark buffers");
   g_mbox_storage.cmd_seq=91;
   check(me_host_init(".")<0 && g_mbox_storage.cmd_seq==91,
         "init refuses to clear an existing module mailbox");
   stop_fail=0;
   me_host_shutdown();
   check(stop_calls==2,"shutdown retries ME reset via module_stop");
   check(frees==2,"confirmed module stop reclaims quarantined buffers");
   /* A stopped/restarted healthy dispatcher gets a full watchdog window. */
   g_me_beat_stale=29;handshake=1;
   check(me_host_init(".")==0,"new module handshakes");
   check(me_host_watchdog_frame()==0,"watchdog stale count resets on restart");
   complete_job=1;
   check(me_host_bench(64,1)==0,"completed benchmark still succeeds");
   check(frees==4,"completed benchmark releases its two allocations");
   me_host_shutdown();
   retain_cases();
   me_host_shutdown();
   for(i=0;i<allocations;i++) free(allocated[i]);
   return 0;
}
'''

USB = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define HANDOFF_PID 0x1c8
#define PSP_USBSTOR_DRIVERNAME "stor"
#define PSP_USBBUS_DRIVERNAME "bus"
#define PSP_O_WRONLY 1
#define PSP_O_CREAT 2
#define PSP_O_TRUNC 4
#define PSP_USB_ACTIVATED 0x200
#define PSP_USB_CABLE_CONNECTED 0x20
#define PSP_USB_CONNECTION_ESTABLISHED 2
#define PSP_POWER_TICK_SUSPEND 1
#define PSP_POWER_TICK_DISPLAY 2
#define PARK_WINDOW_MAX_S 300
#define POLL_MS 200
typedef unsigned SceSize;
struct SceKernelLoadExecVSHParam { unsigned size,args;void *argp;const char *key; };
static int h_enabled=1,h_window_s=1,h_max_runs=20,h_park_s=1;
static int h_usb_active,physical_export,fail_deactivate,escape_now;
static int unsafe_access,exit_calls,stop_calls;
static char h_dir[160],h_result[192],h_cmd[192],h_state[192],h_runs[192];
static char g_dir_base[128]="fixture";
static void access_ms0(void) { if(physical_export)unsafe_access++; }
static int sceIoMkdir(const char *p,int m) {(void)p;(void)m;access_ms0();return 0;}
static int sceIoRemove(const char *p) {(void)p;access_ms0();return 0;}
static int sceIoOpen(const char *p,int f,int m) {(void)p;(void)f;(void)m;access_ms0();return 1;}
static int sceIoWrite(int fd,const void *p,int n) {(void)fd;(void)p;access_ms0();return n;}
static void sceIoClose(int fd) {(void)fd;}
static int h_ms0_forbidden(const char *s) {(void)s;return h_usb_active;}
static void h_note(const char *s,...) {(void)s;if(!h_usb_active)access_ms0();}
static void h_flush_notes(void) {if(!h_usb_active)access_ms0();}
static int h_run_index(void) {access_ms0();return 0;}
static void h_write_run_index(int n) {(void)n;access_ms0();}
static void h_write_result(int n,int e,const char *r,const char *s)
{(void)n;(void)e;(void)r;(void)s;access_ms0();}
static int h_read_small(const char *p,char *b,int n)
{(void)p;(void)n;if(h_usb_active)return -1;access_ms0();strcpy(b,"EXIT");return 4;}
static int h_usb_up(void) {h_usb_active=physical_export=1;return 0;}
static int sceUsbDeactivate(int p) {(void)p;if(fail_deactivate)return -1;physical_export=0;return 0;}
static int sceUsbStop(const char *s,int n,void *p) {(void)s;(void)n;(void)p;stop_calls++;return 0;}
static int sceUsbGetState(void) {return PSP_USB_ACTIVATED;}
static int h_escape_held(int *held) {(void)held;return escape_now;}
static void scePowerTick(int n) {(void)n;}
static void sceKernelDelayThread(int n) {(void)n;}
static void sceKernelExitGame(void) {exit_calls++;}
static void sceKernelLoadExec(const char *p,void *a) {(void)p;(void)a;access_ms0();}
static void sctrlKernelLoadExecVSHMs2(const char *p,void *a) {(void)p;(void)a;access_ms0();}
@FUNCTIONS@
int main(int argc,char **argv)
{
   int baseline=argc>1,e,f;
   (void)argv;
   for(e=0;e<2;e++)for(f=0;f<2;f++)
   {
      escape_now=e;fail_deactivate=f;
      h_usb_active=physical_export=unsafe_access=exit_calls=stop_calls=0;
      handoff_run(0,"fixture");
      printf("USB release escape=%d failure=%d local accesses while exported=%d\n",e,f,unsafe_access);
      if(!baseline)
      {
         assert(unsafe_access==0);
         assert(h_usb_active==f);
         assert(exit_calls==f);
         assert(stop_calls==(f?0:2));
      }
   }
   return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true")
    args = parser.parse_args()
    source = (ROOT / "psp/main_psp.c").read_text(encoding="utf-8")
    selected = "\n\n".join(function(source, name) for name in
                           ("io_thread", "mer_work_heap_free", "me_rend_suspend",
                            "me_rend_release_vmem", "me_standby_down",
                            "pwr_release_all"))
    usb = (ROOT / "psp/usb_handoff.c").read_text(encoding="utf-8")
    usb_selected = "\n\n".join(function(usb, name) for name in ("h_usb_down", "handoff_run"))
    with tempfile.TemporaryDirectory(prefix="gbadhoc-psp-runtime-") as work:
        work = Path(work)
        (work / "pspkernel.h").write_text(SDK)
        (work / "kubridge.h").write_text('#include "pspkernel.h"\n')
        for name, text in (("frontend", FRONT.replace("@FUNCTIONS@", selected)),
                           ("driver", DRIVER),
                           ("usb", USB.replace("@FUNCTIONS@", usb_selected))):
            src = work / (name + ".c")
            binary = work / name
            src.write_text(text)
            subprocess.run(["gcc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra",
                            "-Wno-pointer-to-int-cast", "-Wno-unused-function",
                            "-fsanitize=address,undefined", "-I" + str(work),
                            "-I" + str(ROOT), "-I" + str(ROOT / "frontend-common"),
                            str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)] + (["--baseline"] if args.baseline else []), check=True)


if __name__ == "__main__":
    main()
