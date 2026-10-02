#ifndef MGIFT_SHORTCUT_H
#define MGIFT_SHORTCUT_H
#include <pspctrl.h>

/* The twelve bindable buttons (ctl_map.h CTL_ALL, in SDK spelling). */
#define MGIFT_SHORTCUT_ALL (PSP_CTRL_SELECT | PSP_CTRL_START | PSP_CTRL_UP | \
   PSP_CTRL_RIGHT | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_LTRIGGER | \
   PSP_CTRL_RTRIGGER | PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | \
   PSP_CTRL_SQUARE)

/* Fire once per complete chord, in either press order, and only while NO
 * other bindable button is held. Consume the chord's buttons until all are
 * released so dismissing the chord cannot move the game menu.
 *
 * `chord` is the player's binding (Controls > Mystery Gift; 0 = unbound,
 * never fires).  With the default SELECT+DOWN the conflict set is exactly
 * the list 3.0 spelled out by hand. */
static inline int mgift_shortcut_update_chord(unsigned pad, unsigned chord,
                                              int enabled, unsigned *consumed)
{
   const unsigned conflicts = MGIFT_SHORTCUT_ALL & ~chord;
   if (!enabled || !chord) { *consumed = 0; return 0; }
   if (*consumed)
   {
      if (!(pad & *consumed)) *consumed = 0;
      return 0;
   }
   if ((pad & chord) == chord && !(pad & conflicts))
   {
      *consumed = chord;
      return 1;
   }
   return 0;
}

/* The 3.0 default chord, SELECT+DOWN. */
static inline int mgift_shortcut_update(unsigned pad, int enabled, unsigned *consumed)
{
   return mgift_shortcut_update_chord(pad, PSP_CTRL_SELECT | PSP_CTRL_DOWN,
                                      enabled, consumed);
}
#endif
