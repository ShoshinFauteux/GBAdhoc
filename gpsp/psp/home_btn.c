#include <pspkernel.h>
#include <pspctrl.h>
#include <stdint.h>
#include <string.h>
#include <systemctrl.h>
#include <kubridge.h>

#include "home_btn.h"
#include "fe_evt.h"

#define NID_CTRL_PEEK_POSITIVE 0x3A622550u  /* sceCtrlPeekBufferPositive */
#define NID_IMPOSE_SET_HOME    0x5595A71Au  /* sceImposeSetHomePopup     */
#define NID_IMPOSE_GET_HOME    0x0F341BE4u  /* sceImposeGetHomePopup     */

static void *s_peek, *s_set_popup, *s_get_popup;
static int   s_ready, s_armed, s_tried, s_popup_before = 1;
static SceCtrlData s_pad __attribute__((aligned(64)));

static int kcall(void *fn, uint32_t a1, uint32_t a2)
{
   struct KernelCallArg a;
   memset(&a, 0, sizeof(a));
   a.arg1 = a1;
   a.arg2 = a2;
   if (kuKernelCall(fn, &a) < 0)
      return -1;
   return (int)a.ret1;
}

int home_btn_init(void)
{
   int rc;
   if (s_tried)
      return s_ready;
   s_tried = 1;
   s_peek      = (void *)sctrlHENFindFunction("sceController_Service", "sceCtrl", NID_CTRL_PEEK_POSITIVE);
   s_set_popup = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", NID_IMPOSE_SET_HOME);
   s_get_popup = (void *)sctrlHENFindFunction("sceImpose_Driver", "sceImpose_driver", NID_IMPOSE_GET_HOME);
   if (!s_peek || !s_set_popup)
   {
      fe_evt("home_btn unavailable peek=%p set=%p", s_peek, s_set_popup);
      return 0;
   }
   memset(&s_pad, 0, sizeof(s_pad));
   rc = kcall(s_peek, (uint32_t)(uintptr_t)&s_pad, 1);
   if (rc < 0)
   {
      fe_evt("home_btn unavailable read rc=%d", rc);
      return 0;
   }
   s_ready = 1;
   fe_evt("home_btn ready peek=%p set=%p get=%p", s_peek, s_set_popup, s_get_popup);
   return 1;
}

void home_btn_arm(int on)
{
   if (!s_ready || (on ? 1 : 0) == s_armed)
      return;
   if (on)
   {
      if (s_get_popup)
      {
         int v = kcall(s_get_popup, 0, 0);
         s_popup_before = v >= 0 ? (v != 0) : 1;
      }
      (void)kcall(s_set_popup, 0, 0);
      s_armed = 1;
      fe_evt("home_btn armed (popup was %d)", s_popup_before);
   }
   else
   {
      (void)kcall(s_set_popup, 1u, 0);
      s_armed = 0;
      fe_evt("home_btn released");
   }
}

unsigned home_btn_read(void)
{
   if (!s_armed)
      return 0;
   if (kcall(s_peek, (uint32_t)(uintptr_t)&s_pad, 1) < 0)
      return 0;
   return (s_pad.Buttons & PSP_CTRL_HOME) ? 1u : 0u;
}

int home_btn_armed(void) { return s_armed; }
