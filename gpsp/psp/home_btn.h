/* home_btn.h -- read the HOME button from a user-mode EBOOT (CFW only).
 *
 * The PSP hides HOME from user-mode sceCtrl reads and, when it is pressed,
 * shows the system "exit game?" dialog.  On custom firmware we can do better
 * without turning the whole emulator into a kernel module:
 *   - read the pad once per frame THROUGH the kernel copy of sceCtrl
 *     (kuKernelCall), which keeps the HOME bit;
 *   - switch the system HOME popup off while a game is running so the press
 *     reaches us instead of the dialog.
 * If either piece cannot be found on this firmware, home_btn_arm() leaves the
 * popup alone and home_btn_read() returns 0, so HOME keeps its old meaning
 * (system exit dialog) and the caller falls back to its old gesture. */
#ifndef HOME_BTN_H
#define HOME_BTN_H

/* Look up the kernel entry points and prove a read works.  1 = usable. */
int      home_btn_init(void);
/* 1: take HOME for ourselves (popup off).  0: give it back (popup on).
 * Safe to call repeatedly and when unusable. */
void     home_btn_arm(int on);
/* Current HOME state, 0 or 1.  Always 0 until armed. */
unsigned home_btn_read(void);
int      home_btn_armed(void);

#endif
