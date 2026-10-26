/* ctl_map.h -- user control remapping (docs/CONTROL-REMAP.md).
 *
 * One table says which PSP input drives each GBA/GB button and each emulator
 * shortcut.  It is plain data plus pure functions, so the host test
 * (tools/tests/test_ctl_map.c) exercises exactly the code the PSP runs.
 *
 * WHAT IS NOT IN HERE, ON PURPOSE:
 *   - START+SELECT (hold) opens the in-game menu, and holding it longer ends
 *     the run (ADR-0057).  It is the way back into this very table, so it can
 *     never be rebound, and no shortcut may contain both buttons.
 *   - HOME belongs to the system -- except that Settings > Controls > Menu
 *     can hand it to the in-game menu (ctl_map.menu_home, below; the kernel
 *     side is psp/home/).  HOME itself is never a binding: it cannot be put
 *     on a game button or a shortcut, only chosen as the menu button.
 *   - Navigation INSIDE menus (d-pad, X confirm, O back, the browser's own
 *     keys).  A game binding never reaches a menu, so no mapping can lock the
 *     player out of the screen that undoes it.
 *
 * DEFAULTS ARE THE 3.0 BEHAVIOUR, BIT FOR BIT.  Every trigger rule below is
 * written so that with the default table it reduces to the exact expression
 * main_psp.c used before this existed (the per-action comments in ctl_map.c
 * show the reduction).  The per-frame game mapping is ten AND/ORs, the same
 * work the hard-coded version did. */
#ifndef CTL_MAP_H
#define CTL_MAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Button bits: the PSP SDK's PSP_CTRL_* values (pspctrl.h), restated so this
 * module and its host test need no SDK.  main_psp.c and ui_psp.c check the
 * equality at compile time. */
#define CTL_SELECT    0x0001u
#define CTL_START     0x0008u
#define CTL_UP        0x0010u
#define CTL_RIGHT     0x0020u
#define CTL_DOWN      0x0040u
#define CTL_LEFT      0x0080u
#define CTL_L         0x0100u
#define CTL_R         0x0200u
#define CTL_TRIANGLE  0x1000u
#define CTL_CIRCLE    0x2000u
#define CTL_CROSS     0x4000u
#define CTL_SQUARE    0x8000u
/* THE THUMBSTICK, as four more buttons.  The analog stick is turned into
 * four direction bits by ctl_stick_bits() and from there on is an ordinary
 * input: any action can be bound to one, chords and conflicts work as for
 * any button.  The bits are the four pad.Buttons never uses below bit 16,
 * so a binding still fits the table's uint16_t; the caller clears them from
 * the raw pad word before adding the synthesised ones. */
#define CTL_STICK_UP    0x0002u
#define CTL_STICK_DOWN  0x0004u
#define CTL_STICK_LEFT  0x0400u
#define CTL_STICK_RIGHT 0x0800u
#define CTL_STICK_ALL   0x0C06u
/* The sixteen bindable inputs: twelve buttons and the stick's four
 * directions.  pad.Buttons also carries HOME, HOLD, the WLAN switch, NOTE,
 * SCREEN and the volume keys; none of them is bindable, and every
 * comparison here masks them away first. */
#define CTL_BUTTONS   0xF3F9u           /* the twelve real buttons */
#define CTL_ALL       0xFFFFu
#define CTL_NBUTTONS  16
/* The fixed menu chord.  A shortcut containing both is refused. */
#define CTL_MENU_CHORD (CTL_START | CTL_SELECT)
/* Longest chord a shortcut may use (the longest 3.0 had was four buttons). */
#define CTL_COMBO_MAX 4

/* Actions, in table (and menu, and conflict-priority) order. */
enum
{
   /* GBA / GB buttons: one PSP button each, or none. */
   CTL_GAME_A, CTL_GAME_B, CTL_GAME_L, CTL_GAME_R,
   CTL_GAME_START, CTL_GAME_SELECT,
   CTL_GAME_UP, CTL_GAME_DOWN, CTL_GAME_LEFT, CTL_GAME_RIGHT,
   CTL_GAME_COUNT,
   /* Emulator shortcuts: one button or a chord of up to CTL_COMBO_MAX. */
   CTL_SC_FF = CTL_GAME_COUNT,  /* fast-forward (hold or toggle)          */
   CTL_SC_VIDEO,                /* cycle the video preset                  */
   CTL_SC_SAVE,                 /* quick save, slot 1                      */
   CTL_SC_LOAD,                 /* quick load, slot 1                      */
   CTL_SC_SHOT,                 /* screenshot                              */
   CTL_SC_PAUSE,                /* pause overlay (the wake screen)         */
   CTL_SC_MGIFT,                /* Mystery Gift listener (GBA only)        */
   CTL_SC_CATCH,                /* ME_CATCH self-test: bisect builds only  */
#ifdef REWIND
   /* Rewind (docs/REWIND.md): a LEVEL, read every frame while held.  Its
    * default is Triangle, which in a REWIND build is taken from "Video
    * preset" (that one starts unbound; main_psp.c lets the rewind button
    * cycle the preset when rewind is switched off or cannot be used, so
    * Triangle is never a dead button).  The LAST SHORTCUT: an existing
    * CONFIG.INI that binds Triangle to something else keeps that.  (The
    * turbo rows after it have no default, so they cannot contest it.) */
   CTL_SC_REWIND,
#endif
   /* One past the last shortcut.  The longer-chord scan (ctl_yields) stops
    * here: the rows below are not shortcuts and never take part in it. */
   CTL_SC_END,
   /* TURBO BUTTONS (docs/CONTROL-REMAP.md section 13): one PSP input each,
    * like a game button, UNBOUND by default.  Two for A and two for B, each
    * with its own rate and mode (ctl_map.turbo_half / turbo_toggle).  They
    * come after every other action, so an existing CONFIG.INI resolves its
    * duplicates exactly as it did before these rows existed. */
   CTL_TURBO_A1 = CTL_SC_END,
   CTL_TURBO_A2,
   CTL_TURBO_B1,
   CTL_TURBO_B2,
   CTL_ACTIONS
};
#define CTL_TURBO_FIRST CTL_TURBO_A1
#define CTL_TURBO_N     4

/* ctl_action_info.flags */
#define CTL_F_GAME    0x01u   /* a game button: exactly one PSP button      */
#define CTL_F_YIELD   0x02u   /* does not fire while a LONGER shortcut that
                               * contains its whole chord is held          */
#define CTL_F_TURBO   0x04u   /* a turbo button: exactly one PSP button;
                               * retro_id is the game button it presses    */

typedef struct
{
   const char *key;        /* CONFIG.INI key                               */
   const char *label;      /* menu label                                   */
   uint16_t    def;        /* factory default (A/B: see ctl_default)       */
   uint8_t     retro_id;   /* libretro joypad id (game buttons only)       */
   uint8_t     flags;      /* CTL_F_*                                      */
} ctl_action_info;

typedef struct
{
   uint16_t bind[CTL_ACTIONS];  /* PSP button mask; 0 = unbound           */
   /* Sticky "this key is in CONFIG.INI" flag.  A binding is written when it
    * differs from its default OR its key is already in the file -- so a
    * player who never remaps gets a byte-identical config.ini, and a reset
    * still overwrites a custom value that is on the card. */
   uint8_t  keep[CTL_ACTIONS];
   /* THE MENU BUTTON (CONFIG.INI `menu_button`, docs/CONTROL-REMAP.md section
    * 10).  CTL_MENU_START_SELECT (the default, 3.0's behaviour byte for byte):
    * START+SELECT held ~1/4 s opens the in-game menu.  CTL_MENU_HOME: in game,
    * HOME opens it instead and a START+SELECT tap reaches the game.  Holding
    * START+SELECT ~1.5 s ends the run in BOTH modes.  `menu_keep` is the same
    * sticky in-the-file flag as keep[]: the key is written only when it is not
    * the default or is already on the card. */
   uint8_t  menu_home;
   uint8_t  menu_keep;
   /* TURBO BEHAVIOUR, one pair per turbo action (index: action -
    * CTL_TURBO_FIRST).  turbo_half is the pulse's half period in EMULATED
    * frames: 1 = 30 presses a second (1 on, 1 off), 2 = 15 (the default),
    * 3 = 10.  turbo_toggle: 0 = Hold (pulses while held, the default), 1 =
    * Toggle (a press starts auto-fire, the next press stops it).  The keep
    * flags are the same sticky in-the-file rule as keep[]. */
   uint8_t  turbo_half[CTL_TURBO_N];
   uint8_t  turbo_toggle[CTL_TURBO_N];
   uint8_t  turbo_rate_keep[CTL_TURBO_N];
   uint8_t  turbo_mode_keep[CTL_TURBO_N];
} ctl_map;

#define CTL_MENU_START_SELECT 0
#define CTL_MENU_HOME         1

/* THE THUMBSTICK MODE (CONFIG.INI `stick`, Settings > Controls > Thumbstick).
 *   OFF     the stick does nothing: 3.5.0 and earlier, and the default.
 *   MOVE    the stick is a second d-pad: its directions are the d-pad's
 *           bits, so it does whatever the d-pad is bound to.
 *   CUSTOM  its directions are the CTL_STICK_* inputs, bindable like any
 *           button on the Controls page.
 * A binding that names a stick direction is kept in every mode and simply
 * never fires outside CUSTOM. */
#define CTL_STICK_OFF    0
#define CTL_STICK_MOVE   1
#define CTL_STICK_CUSTOM 2
const char *ctl_stick_name(int mode);          /* "off" / "move" / "custom" */
int ctl_stick_parse(const char *s, int *out);  /* 0 and *out, or -1 */
/* The stick's position (pspctrl Lx / Ly: 0..255, 128 centred, Ly 0 = up)
 * as pad bits for `mode`.  Each axis engages at CTL_STICK_ON from centre and
 * lets go below CTL_STICK_OFF_AT, so a direction held at the edge of the
 * dead zone does not chatter and a worn stick resting off-centre stays
 * quiet; the axes are independent, so diagonals are two directions.
 * `state` (start it at 0) carries the engaged directions between calls. */
#define CTL_STICK_ON      72
#define CTL_STICK_OFF_AT  52
unsigned ctl_stick_bits(int mode, int lx, int ly, unsigned *state);
/* 1 if `mask` is bound to the stick alone: one or more stick directions
 * and no real button. */
int ctl_stick_only(unsigned mask);
/* THE PAD WORD every reader of the controls uses: pad.Buttons with the four
 * bits it never sets cleared, plus ctl_stick_bits() for `mode` (OFF adds
 * nothing and zeroes `state`).  One function so that no pad loop -- the game
 * loop, the browser's own Settings loop -- can leave the stick out: the
 * Controls page captures a binding from this word, and a loop that passed
 * raw pad.Buttons could never bind a stick direction. */
unsigned ctl_pad_word(unsigned buttons, int mode, int lx, int ly,
                      unsigned *state);
/* CONFIG.INI spelling of a menu mode ("start_select" / "home"). */
const char *ctl_menu_name(int menu_home);
/* "start_select", "START+SELECT", "home" (any case, surrounding blanks
 * allowed) -> 0 and *out; anything else, empty included, -> -1. */
int ctl_menu_parse(const char *s, int *out);

/* Error codes from ctl_check / ctl_assign. */
enum
{
   CTL_OK = 0,
   CTL_E_ACTION,     /* no such action, or not in this build              */
   CTL_E_BITS,       /* a button that cannot be bound                     */
   CTL_E_SINGLE,     /* a game or turbo button takes exactly one button   */
   CTL_E_RESERVED,   /* contains START+SELECT, the menu chord             */
   CTL_E_TOO_MANY    /* more than CTL_COMBO_MAX buttons                   */
};

const ctl_action_info *ctl_info(int a);
/* 0 for an action this build does not have (the catch self-test outside a
 * GPSP_CATCH_SELFTEST build).  Such an action is held unbound, is never read
 * from or written to CONFIG.INI, and never conflicts with anything. */
int  ctl_available(int a);
/* The factory default for `a` (A on Circle, B on Cross).  There is no other
 * base layout: 3.0's "A/B buttons" setting (CONFIG.INI `btn_swap`) is gone,
 * and ctl_load_ini() converts an old swapped config into bind_a/bind_b. */
unsigned ctl_default(int a);
/* Every binding to its default, keep flags cleared (boot). */
void ctl_defaults(ctl_map *m);
/* "Reset to defaults": factory table, keep flags PRESERVED.  The menu button
 * returns to START+SELECT too: it lives on the same page. */
void ctl_reset(ctl_map *m);
int  ctl_is_default(const ctl_map *m);

int  ctl_popcount(unsigned mask);
/* Is `mask` a legal binding for `a`?  0 (unbound) always is. */
int  ctl_check(int a, unsigned mask);
const char *ctl_error_text(int err);

/* "SELECT+L", "l + select", "NONE" -> mask.  Case-insensitive, any order,
 * spaces allowed around '+'.  Returns 0 and sets *out, or -1 for anything
 * else -- including an EMPTY value: a blank key must fall back to the
 * default, never quietly unbind (only the word NONE unbinds). */
int  ctl_parse(const char *s, unsigned *out);
/* Canonical text in a fixed button order ("SELECT+L", "NONE").  `names`
 * (CTL_NBUTTONS entries, ctl_button_order() order) and `none` override the
 * words, for the menu's glyphs; NULL = the CONFIG.INI spelling.  Returns the
 * length written (always NUL-terminated when sz > 0). */
size_t ctl_format(unsigned mask, char *buf, size_t sz,
                  const char *const *names, const char *none);
/* The bit for display slot i (0..CTL_NBUTTONS-1). */
unsigned ctl_button_order(int i);

/* The action other than `except` bound to exactly `mask`, or -1. */
int  ctl_find(const ctl_map *m, unsigned mask, int except);
/* THE CONFLICT RULE.  Bind `a` to `mask` (0 = unbind).  If another action
 * already has exactly that input, the NEW binding takes it and the old
 * holder is left UNBOUND; its index goes to *stolen (else -1).  Nothing
 * else moves.  Returns CTL_OK or a CTL_E_* with the map unchanged. */
int  ctl_assign(ctl_map *m, int a, unsigned mask, int *stolen);
/* Load-time repair: every binding validated (an illegal one returns to its
 * default) and every duplicate settled by the same rule the menu applies --
 * an explicit (non-default) binding beats a default one, and between equals
 * the earlier action in the table wins; the loser is unbound.  `note` (may be
 * NULL) hears each change: loser, winner (-1 when the binding was illegal).
 * Returns the number of bindings changed. */
int  ctl_repair(ctl_map *m,
                void (*note)(void *user, int a, int winner), void *user);

/* CONFIG.INI.  Load validates every key: a missing, empty or unparseable
 * value, or a binding the action cannot take, falls back to the DEFAULT.
 * Returns the number of values rejected.  Save writes only what the keep
 * rule above asks for; returns -1 if any write failed.
 *
 * MIGRATION (docs/CONTROL-REMAP.md, "btn_swap").  A config whose legacy
 * `btn_swap` is nonzero and which lacks bind_a or bind_b is one 3.0 (or an
 * earlier 3.1 candidate) wrote for a player who swapped A and B.  It is
 * loaded exactly as those builds loaded it -- the swapped pair is the base
 * that missing/invalid A/B keys and the repair tie-break resolve to -- and
 * A and B are then marked keep, so the next save writes the effective pair
 * as explicit bind_a/bind_b.  From then on both keys are in the file and
 * `btn_swap` is never consulted again. */
int  ctl_load_ini(ctl_map *m, const char *path);
int  ctl_save_ini(ctl_map *m, const char *path);
/* The value pcfg_save() writes to the legacy `btn_swap` key: 1 when A is on
 * Cross and B on Circle, else 0.  WRITE-ONLY: it exists so that a 3.0 build
 * (which ignores bind_*) still finds the player's A/B layout after a
 * downgrade, and so that a player who never remapped keeps a CONFIG.INI
 * byte-identical to 3.0's.  This build never acts on it: a 1 here implies
 * A and B differ from their defaults, so both bind keys are written beside
 * it, and ctl_load_ini() ignores `btn_swap` whenever both are present. */
int  ctl_legacy_swap(const ctl_map *m);

/* The per-frame game mapping: PSP buttons -> libretro joypad mask.  With the
 * default table this is exactly 3.0's plat_input_bitmask. */
uint32_t ctl_game_mask(const ctl_map *m, unsigned pad);

/* THE LONGER-CHORD RULE.  1 if `a` carries CTL_F_YIELD and some OTHER
 * shortcut's chord strictly contains `a`'s whole chord and is fully held:
 * `a` must stand down this frame (SELECT+L does not quick-save while
 * L+R+SELECT is taking a screenshot).  0 for actions without the flag. */
int  ctl_yields(const ctl_map *m, int a, unsigned pad);

/* ---- the shortcuts that fired this frame ---------------------------------
 * One call per frame from the main loop; each bit is the binding's chord in
 * that action's trigger SHAPE (below), longer-chord rule applied.  Context
 * gates -- menu open, script running, wireless session, console -- stay with
 * the caller.  Mystery Gift is stateful (it consumes its buttons) and lives
 * in psp/mgift_shortcut.h. */
#define CTL_FIRE_FF_HELD   0x001u   /* FF chord held (hold mode)            */
#define CTL_FIRE_FF_PRESS  0x002u   /* FF chord pressed (toggle mode)       */
#define CTL_FIRE_VIDEO     0x004u
#define CTL_FIRE_SAVE      0x008u
#define CTL_FIRE_LOAD      0x010u   /* never together with SAVE (3.0 else-if) */
#define CTL_FIRE_SHOT      0x020u
#define CTL_FIRE_PAUSE     0x040u
#define CTL_FIRE_CATCH     0x080u   /* level, while held (bisect builds)    */
#ifdef REWIND
#define CTL_FIRE_REWIND    0x100u   /* level, while held                    */
#define CTL_FIRE_REWIND_PRESS 0x200u /* its press edge                      */
#endif
unsigned ctl_shortcuts(const ctl_map *m, unsigned pad, unsigned pad_new);

/* ---- turbo buttons (docs/CONTROL-REMAP.md section 13) --------------------
 *
 * A turbo button presses A or B for the player, on and off, at a fixed rate.
 * The pulse is counted in EMULATED FRAMES, never in wall time: the same pad
 * input on the same frames always gives the game the same buttons, fast-
 * forward makes it faster with the game, and nothing here reads a clock.
 *
 * ZERO COST WHEN UNUSED.  ctl_game_mask() does not know turbo exists.  The
 * platform adds ctl_turbo_mask() only while ctl_turbo.live is set, and steps
 * ctl_turbo_pad() only while something is bound (ctl_turbo_bound) or live.
 *
 * The state is NOT part of the emulated machine: it is derived from the pad
 * each frame and is never saved, loaded or rewound. */
#define CTL_TURBO_HALF_MIN 1      /* 30 presses a second */
#define CTL_TURBO_HALF_DEF 2      /* 15 */
#define CTL_TURBO_HALF_MAX 3      /* 10 */

typedef struct
{
   uint32_t t0[CTL_TURBO_N];    /* emulated frame its pulse train began on  */
   uint8_t  run[CTL_TURBO_N];   /* firing: held (Hold) or latched (Toggle)   */
   uint8_t  live;               /* any run[] set: the platform's one test    */
} ctl_turbo;

/* Presses per second at 60 frames a second: 30 / 15 / 10. */
int  ctl_turbo_rate(int half);
/* The next setting in the cycle 10 -> 15 -> 30 -> 10 (dir < 0: backwards). */
int  ctl_turbo_half_step(int half, int dir);
/* "10" / "15" / "30" -> 0 and *half; anything else -> -1. */
int  ctl_turbo_rate_parse(const char *s, int *half);
/* "hold" / "toggle" (any case, blanks allowed) -> 0 and *toggle; else -1. */
int  ctl_turbo_mode_parse(const char *s, int *toggle);
const char *ctl_turbo_mode_name(int toggle);          /* "hold" / "toggle" */
/* CONFIG.INI key of turbo action `a`'s rate (which = 0) or mode (1), or
 * NULL when `a` is not a turbo action. */
const char *ctl_turbo_key(int a, int which);
/* Nonzero if any turbo action is bound. */
static inline unsigned ctl_turbo_bound(const ctl_map *m)
{
   return (unsigned)(m->bind[CTL_TURBO_A1] | m->bind[CTL_TURBO_A2] |
                     m->bind[CTL_TURBO_B1] | m->bind[CTL_TURBO_B2]);
}
/* ONE CALL PER PAD SAMPLE while the game is running: `pad` is what is held,
 * `pad_new` what went down since the last sample, `frame` the emulated frame
 * about to run.  Hold starts a train on the frame the button goes down (so a
 * tap is always at least one press) and ends it on release; Toggle flips on
 * each press.  enabled == 0 (a link session): everything stops and nothing
 * starts. */
void ctl_turbo_pad(const ctl_map *m, ctl_turbo *t, unsigned pad,
                   unsigned pad_new, uint32_t frame, int enabled);
/* The libretro joypad bits the running trains press on emulated frame
 * `frame`.  A pure function of its arguments: any number of reads of one
 * frame agree.  OR it into ctl_game_mask(): a turbo button never takes a
 * press away, so normal A held with Turbo A is A held. */
uint32_t ctl_turbo_mask(const ctl_map *m, const ctl_turbo *t, uint32_t frame);
/* Everything stops: the menu opened, a state was loaded, the game is being
 * left, the console slept, a rewind is held.  A Hold button still down
 * starts a fresh train at the next ctl_turbo_pad(); a Toggle stays off until
 * it is pressed again. */
void ctl_turbo_stop(ctl_turbo *t);

/* ---- trigger shapes ------------------------------------------------------
 * pad = buttons held now, pad_new = pressed this frame (pad & ~previous). */

/* Every button of the chord held. */
static inline int ctl_held(unsigned pad, unsigned m)
{
   return m && (pad & m) == m;
}

/* Chord complete AND one of its buttons went down this frame: fires once per
 * press, whatever order the buttons arrive in. */
static inline int ctl_edge(unsigned pad, unsigned pad_new, unsigned m)
{
   return m && (pad_new & m) && (pad & m) == m;
}

/* Modifier chord: SELECT/START inside it are MODIFIERS that must already be
 * down; the press of any other button of the chord fires it.  SELECT+L fires
 * on L with SELECT held, not on SELECT with L held -- 3.0's quick-save shape.
 * A chord made only of modifiers fires like ctl_edge. */
static inline int ctl_trigger(unsigned pad, unsigned pad_new, unsigned m)
{
   unsigned trig = m & ~CTL_MENU_CHORD;
   if (!trig)
      trig = m;
   return m && (pad_new & trig) && (pad & m) == m;
}

#ifdef __cplusplus
}
#endif

#endif /* CTL_MAP_H */
