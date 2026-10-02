/* home_link.h -- the ONE shared contract between the EBOOT (user mode) and
 * gbadhoc_home.prx (kernel mode), for Settings > Controls > Menu = HOME
 * (docs/CONTROL-REMAP.md, section 10).
 *
 * WHY A KERNEL MODULE AT ALL.  A user-mode sceCtrl read never shows HOME (the
 * firmware masks it out of every user buffer), and the HOME popup -- "Quit the
 * game?" -- is drawn by the system the moment HOME goes down.  Only kernel code
 * can see the button, and only sceImpose's kernel export can switch the popup
 * off.  So the module owns those two calls and nothing else; every decision
 * about WHEN is the EBOOT's, written here.
 *
 * The block lives in the EBOOT's own .bss (64-byte aligned, static, so its
 * lifetime is the process).  Both sides run on the main CPU, so plain
 * volatile access is coherent; there is no Media Engine involvement at all.
 *
 * SAFETY RULES, each load-bearing:
 *   - The module suppresses the popup only while ALL of: `armed` holds
 *     HOME_ARMED (a magic, not a bool, so stray memory cannot arm it), the
 *     EBOOT's `beat` has moved within `stall_ms`, and the module is running.
 *     Anything else -- a frozen main loop, a menu that is not the game, a
 *     module_stop -- gives HOME back to the system.
 *   - module_stop always re-enables the popup.  And no state survives the
 *     process anyway: every way out of a game (the exit dialog, our own exit,
 *     a relaunch, a crash back to the XMB) is a LoadExec, which reboots the
 *     kernel and re-initialises sceImpose with the popup on.
 */
#ifndef HOME_LINK_H
#define HOME_LINK_H

#define HOME_LINK_MAGIC   0x484C4E4Bu   /* "HLNK": the EBOOT, before start   */
#define HOME_LINK_VERSION 1u            /* written by the module's thread     */
#define HOME_ARMED        0x484F4D45u   /* "HOME": suppress while beating     */
#define HOME_STALL_MS_DEFAULT 2000u     /* the owner's "about 2 s"            */

typedef struct home_link
{
   /* --- written by the EBOOT ------------------------------------------- */
   volatile unsigned int magic;      /* HOME_LINK_MAGIC before module start  */
   volatile unsigned int beat;       /* ++ once per main-loop iteration      */
   volatile unsigned int armed;      /* HOME_ARMED while a game owns HOME    */
   volatile unsigned int stall_ms;   /* fallback threshold; 0 = default      */
   /* --- written by the module ------------------------------------------ */
   volatile unsigned int version;    /* HOME_LINK_VERSION once polling       */
   volatile unsigned int kbeat;      /* ++ every poll: the service is alive  */
   volatile unsigned int home_seq;   /* ++ per HOME press while suppressed   */
   volatile unsigned int home_us;    /* sceKernelGetSystemTimeLow of it      */
   volatile unsigned int popup_off;  /* 1 while the system popup is off      */
   volatile unsigned int fallbacks;  /* times a stall gave HOME back         */
   volatile int          imp_rc;     /* last sceImposeSetHomePopup result    */
   volatile int          popup_orig; /* sceImposeGetHomePopup at start       */
   volatile unsigned int fail;       /* 1 once a SetHomePopup(0) was refused */
   volatile unsigned int pad_[3];
} home_link;

#endif /* HOME_LINK_H */
