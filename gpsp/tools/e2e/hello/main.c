/* Harness smoke-test EBOOT: proves a sandboxed PPSSPP instance boots homebrew
 * and that ms0: file I/O is observable from the host (the harness log channel). */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <stdio.h>

PSP_MODULE_INFO("gpspadhoc_hello", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

static int exit_cb(int arg1, int arg2, void *common)
{
  sceKernelExitGame();
  return 0;
}

static int cb_thread(SceSize args, void *argp)
{
  int cbid = sceKernelCreateCallback("exit_cb", exit_cb, NULL);
  sceKernelRegisterExitCallback(cbid);
  sceKernelSleepThreadCB();
  return 0;
}

int main(void)
{
  int thid = sceKernelCreateThread("cb_thread", cb_thread, 0x11, 0xFA0, 0, NULL);
  if (thid >= 0)
    sceKernelStartThread(thid, 0, NULL);

  pspDebugScreenInit();
  pspDebugScreenPrintf("gpsp-adhoc harness hello\n");

  FILE *f = fopen("ms0:/PSP/GAME/hello/harness.log", "w");
  if (f) {
    fprintf(f, "EVT boot_ok\n");
    fprintf(f, "EVT clock=%d\n", scePowerGetCpuClockFrequency());
    fprintf(f, "EVT exit code=0\n");
    fclose(f);
  }
  pspDebugScreenPrintf("log written, exiting in ~3s\n");

  for (int i = 0; i < 180; i++)
    sceDisplayWaitVblankStart();

  sceKernelExitGame();
  return 0;
}
