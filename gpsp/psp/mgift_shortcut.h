#ifndef MGIFT_SHORTCUT_H
#define MGIFT_SHORTCUT_H
#include <pspctrl.h>

/* Fire once per complete chord, in either press order. Consume both buttons
 * until both are released so dismissing the chord cannot move the game menu. */
static inline int mgift_shortcut_update(unsigned pad, int enabled, unsigned *consumed)
{
   const unsigned chord = PSP_CTRL_SELECT | PSP_CTRL_DOWN;
   const unsigned conflicts = PSP_CTRL_START | PSP_CTRL_UP | PSP_CTRL_LEFT |
      PSP_CTRL_RIGHT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_TRIANGLE |
      PSP_CTRL_SQUARE | PSP_CTRL_CROSS | PSP_CTRL_CIRCLE;
   if (!enabled) { *consumed = 0; return 0; }
   if (*consumed)
   {
      if (!(pad & chord)) *consumed = 0;
      return 0;
   }
   if ((pad & chord) == chord && !(pad & conflicts))
   {
      *consumed = chord;
      return 1;
   }
   return 0;
}
#endif
