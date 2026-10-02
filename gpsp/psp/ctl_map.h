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
/* The twelve bindable buttons.  pad.Buttons also carries HOME, HOLD, the
 * WLAN switch, NOTE, SCREEN and the volume keys; none of them is bindable,
 * and every comparison here masks them away first. */
#define CTL_ALL       0xF3F9u
#define CTL_NBUTTONS  12
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
   CTL_ACTIONS
};

/* ctl_action_info.flags */
#define CTL_F_GAME    0x01u   /* a game button: exactly one PSP button      */
#define CTL_F_YIELD   0x02u   /* does not fire while a LONGER shortcut that
                               * contains its whole chord is held          */

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
} ctl_map;

#define CTL_MENU_START_SELECT 0
#define CTL_MENU_HOME         1
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
   CTL_E_SINGLE,     /* a game button takes exactly one button            */
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
unsigned ctl_shortcuts(const ctl_map *m, unsigned pad, unsigned pad_new);

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
