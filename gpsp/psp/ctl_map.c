/* ctl_map.c -- user control remapping (see ctl_map.h, docs/CONTROL-REMAP.md).
 *
 * Pure: no PSP headers, no globals.  Compiled into the EBOOT and, unchanged,
 * into the host test. */
#include <stdio.h>
#include <string.h>

#include "ctl_map.h"
#include "fe_util.h"
#include "fe_evt.h"

/* libretro joypad ids (libretro.h RETRO_DEVICE_ID_JOYPAD_*). */
enum { RJ_B = 0, RJ_SELECT = 2, RJ_START = 3, RJ_UP = 4, RJ_DOWN = 5,
       RJ_LEFT = 6, RJ_RIGHT = 7, RJ_A = 8, RJ_L = 10, RJ_R = 11 };

/* THE TABLE.  Order is menu order and the conflict tie-break.  Keys are part
 * of the CONFIG.INI format: renaming one orphans every player's binding for
 * it (the old key would be reported cfg_unknown and the action would quietly
 * return to its default), so treat them as an interface.
 *
 * How each default reproduces 3.0 exactly (pad = g_pad, new = pad_new):
 *   game buttons  `if (pad & bind) mask |= 1 << id` == the old if-chain;
 *                 its btn_swap branch is now an explicit bind_a/bind_b
 *                 pair, written by the load-time migration (ctl_load_ini).
 *   FF            ctl_held(pad, SQUARE)          == (pad & SQUARE) != 0
 *                 ctl_edge(pad, new, SQUARE)     == (new & SQUARE) != 0
 *                 no default shortcut contains SQUARE, so YIELD never fires.
 *   Video preset  ctl_edge(pad, new, TRIANGLE)   == (new & TRIANGLE) != 0
 *                 (no YIELD: 3.0 cycled the preset even on SELECT+TRIANGLE,
 *                 and the defaults must not change that.)
 *   Quick save    ctl_trigger(pad, new, SELECT|L) == SELECT held && new L,
 *                 and YIELD to the L+R+SELECT screenshot == "R not held".
 *   Quick load    the same with L and R exchanged.
 *   Screenshot    ctl_edge(pad, new, L|R|SELECT) && !(pad & START & ~bind)
 *                 == the ADR-0069 chord test; nothing contains it.
 *   Pause screen  ctl_trigger(pad, new, SELECT|TRIANGLE) == SELECT held &&
 *                 new TRIANGLE; nothing contains it.
 *   Mystery Gift  mgift_shortcut_update_chord(.., SELECT|DOWN, ..) with
 *                 conflicts = every other bindable button == the old list.
 *   Catch test    ctl_held(pad, L|R|UP|TRIANGLE); bisect builds only. */
static const ctl_action_info INFO[CTL_ACTIONS] = {
   { "bind_a",            "A",              CTL_CIRCLE,   RJ_A,      CTL_F_GAME },
   { "bind_b",            "B",              CTL_CROSS,    RJ_B,      CTL_F_GAME },
   { "bind_l",            "L",              CTL_L,        RJ_L,      CTL_F_GAME },
   { "bind_r",            "R",              CTL_R,        RJ_R,      CTL_F_GAME },
   { "bind_start",        "Start",          CTL_START,    RJ_START,  CTL_F_GAME },
   { "bind_select",       "Select",         CTL_SELECT,   RJ_SELECT, CTL_F_GAME },
   { "bind_up",           "Up",             CTL_UP,       RJ_UP,     CTL_F_GAME },
   { "bind_down",         "Down",           CTL_DOWN,     RJ_DOWN,   CTL_F_GAME },
   { "bind_left",         "Left",           CTL_LEFT,     RJ_LEFT,   CTL_F_GAME },
   { "bind_right",        "Right",          CTL_RIGHT,    RJ_RIGHT,  CTL_F_GAME },
   { "bind_ff",           "Fast-forward",   CTL_SQUARE,   0,         CTL_F_YIELD },
   { "bind_video",        "Video preset",   CTL_TRIANGLE, 0,         0 },
   { "bind_quicksave",    "Quick save",     CTL_SELECT | CTL_L, 0,   CTL_F_YIELD },
   { "bind_quickload",    "Quick load",     CTL_SELECT | CTL_R, 0,   CTL_F_YIELD },
   { "bind_screenshot",   "Screenshot",     CTL_L | CTL_R | CTL_SELECT, 0,
                                                                     CTL_F_YIELD },
   { "bind_pause",        "Pause screen",   CTL_SELECT | CTL_TRIANGLE, 0,
                                                                     CTL_F_YIELD },
   { "bind_mystery_gift", "Mystery Gift",   CTL_SELECT | CTL_DOWN, 0, 0 },
   { "bind_catch_test",   "Crash self-test",
                          CTL_L | CTL_R | CTL_UP | CTL_TRIANGLE, 0,  CTL_F_YIELD },
};

/* Display / CONFIG.INI order: modifiers first, so a chord reads the way it
 * is pressed ("SELECT+L": hold SELECT, press L). */
static const struct { unsigned bit; const char *name; } BUTTONS[CTL_NBUTTONS] = {
   { CTL_SELECT, "SELECT" }, { CTL_START, "START" },
   { CTL_L, "L" }, { CTL_R, "R" },
   { CTL_UP, "UP" }, { CTL_DOWN, "DOWN" },
   { CTL_LEFT, "LEFT" }, { CTL_RIGHT, "RIGHT" },
   { CTL_TRIANGLE, "TRIANGLE" }, { CTL_CIRCLE, "CIRCLE" },
   { CTL_CROSS, "CROSS" }, { CTL_SQUARE, "SQUARE" },
};

const ctl_action_info *ctl_info(int a)
{
   return (a >= 0 && a < CTL_ACTIONS) ? &INFO[a] : NULL;
}

int ctl_available(int a)
{
   if (a < 0 || a >= CTL_ACTIONS)
      return 0;
#ifndef GPSP_CATCH_SELFTEST
   if (a == CTL_SC_CATCH)
      return 0;
#endif
   return 1;
}

/* The default `a` resolves to while a config is being LOADED.  `legacy_swap`
 * is nonzero only inside ctl_load_ini(), for a config that still relies on
 * 3.0's btn_swap (see there); everywhere else the factory table is the only
 * default there is. */
static unsigned load_default(int a, int legacy_swap)
{
   if (!ctl_available(a))
      return 0;
   if (legacy_swap && a == CTL_GAME_A)
      return CTL_CROSS;
   if (legacy_swap && a == CTL_GAME_B)
      return CTL_CIRCLE;
   return INFO[a].def;
}

unsigned ctl_default(int a)
{
   return load_default(a, 0);
}

static void defaults_in(ctl_map *m, int legacy_swap)
{
   int a;
   for (a = 0; a < CTL_ACTIONS; a++)
   {
      m->bind[a] = (uint16_t)load_default(a, legacy_swap);
      m->keep[a] = 0;
   }
   m->menu_home = CTL_MENU_START_SELECT;
   m->menu_keep = 0;
}

void ctl_defaults(ctl_map *m)
{
   defaults_in(m, 0);
}

void ctl_reset(ctl_map *m)
{
   int a;
   for (a = 0; a < CTL_ACTIONS; a++)
      m->bind[a] = (uint16_t)ctl_default(a);
   m->menu_home = CTL_MENU_START_SELECT;
}

int ctl_is_default(const ctl_map *m)
{
   int a;
   for (a = 0; a < CTL_ACTIONS; a++)
      if (ctl_available(a) && m->bind[a] != ctl_default(a))
         return 0;
   return m->menu_home == CTL_MENU_START_SELECT;
}

int ctl_popcount(unsigned mask)
{
   int n = 0;
   for (; mask; mask &= mask - 1)
      n++;
   return n;
}

int ctl_check(int a, unsigned mask)
{
   if (!ctl_available(a))
      return CTL_E_ACTION;
   if (!mask)
      return CTL_OK;
   if (mask & ~CTL_ALL)
      return CTL_E_BITS;
   if ((INFO[a].flags & CTL_F_GAME) && ctl_popcount(mask) != 1)
      return CTL_E_SINGLE;
   if ((mask & CTL_MENU_CHORD) == CTL_MENU_CHORD)
      return CTL_E_RESERVED;
   if (ctl_popcount(mask) > CTL_COMBO_MAX)
      return CTL_E_TOO_MANY;
   return CTL_OK;
}

const char *ctl_error_text(int err)
{
   switch (err)
   {
   case CTL_OK:         return "ok";
   case CTL_E_ACTION:   return "not available in this build";
   case CTL_E_BITS:     return "that button cannot be bound";
   case CTL_E_SINGLE:   return "game buttons take one button";
   case CTL_E_RESERVED: return "START+SELECT is reserved for the menu";
   case CTL_E_TOO_MANY: return "at most 4 buttons";
   default:             return "invalid";
   }
}

/* CONFIG.INI key of the menu button (ctl_map.menu_home). */
#define CTL_MENU_KEY "menu_button"

static int up(int c)
{
   return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

/* Case-insensitive compare of [s, s+n) against a NUL-terminated name. */
static int name_eq(const char *s, size_t n, const char *name)
{
   size_t i;
   for (i = 0; i < n; i++)
      if (!name[i] || up((unsigned char)s[i]) != name[i])
         return 0;
   return name[n] == '\0';
}

static unsigned button_bit(const char *s, size_t n)
{
   int i;
   for (i = 0; i < CTL_NBUTTONS; i++)
      if (name_eq(s, n, BUTTONS[i].name))
         return BUTTONS[i].bit;
   /* pspctrl.h spells the shoulders this way; accept it. */
   if (name_eq(s, n, "LTRIGGER")) return CTL_L;
   if (name_eq(s, n, "RTRIGGER")) return CTL_R;
   return 0;
}

const char *ctl_menu_name(int menu_home)
{
   return menu_home ? "home" : "start_select";
}

int ctl_menu_parse(const char *s, int *out)
{
   const char *e;
   size_t n;
   if (!s || !out)
      return -1;
   while (*s == ' ' || *s == '\t')
      s++;
   e = s + strlen(s);
   while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                    e[-1] == '\n'))
      e--;
   n = (size_t)(e - s);
   if (name_eq(s, n, "START_SELECT") || name_eq(s, n, "START+SELECT"))
      *out = CTL_MENU_START_SELECT;
   else if (name_eq(s, n, "HOME"))
      *out = CTL_MENU_HOME;
   else
      return -1;
   return 0;
}

int ctl_parse(const char *s, unsigned *out)
{
   unsigned mask = 0;
   const char *p = s;

   if (!s || !out)
      return -1;
   while (*p == ' ' || *p == '\t')
      p++;
   {
      /* NONE, alone, is the only way to say "unbound". */
      const char *e = p + strlen(p);
      while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                       e[-1] == '\n'))
         e--;
      if (name_eq(p, (size_t)(e - p), "NONE"))
      {
         *out = 0;
         return 0;
      }
   }
   for (;;)
   {
      const char *t, *te;
      unsigned bit;
      while (*p == ' ' || *p == '\t')
         p++;
      t = p;
      while (*p && *p != '+' && *p != ' ' && *p != '\t' && *p != '\r' &&
             *p != '\n')
         p++;
      te = p;
      if (te == t)
         return -1;                     /* empty token: "", "L+", "+L"   */
      bit = button_bit(t, (size_t)(te - t));
      if (!bit || (mask & bit))
         return -1;                     /* unknown or repeated button     */
      mask |= bit;
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
         p++;
      if (!*p)
         break;
      if (*p != '+')
         return -1;                     /* junk between buttons           */
      p++;
   }
   *out = mask;
   return 0;
}

unsigned ctl_button_order(int i)
{
   return (i >= 0 && i < CTL_NBUTTONS) ? BUTTONS[i].bit : 0;
}

size_t ctl_format(unsigned mask, char *buf, size_t sz,
                  const char *const *names, const char *none)
{
   size_t o = 0;
   int i, first = 1;

   if (!buf || !sz)
      return 0;
   buf[0] = '\0';
   if (!(mask & CTL_ALL))
   {
      snprintf(buf, sz, "%s", none ? none : "NONE");
      return strlen(buf);
   }
   for (i = 0; i < CTL_NBUTTONS; i++)
   {
      int n;
      if (!(mask & BUTTONS[i].bit))
         continue;
      n = snprintf(buf + o, sz - o, "%s%s", first ? "" : "+",
                   names ? names[i] : BUTTONS[i].name);
      if (n < 0 || (size_t)n >= sz - o)
      {
         buf[sz - 1] = '\0';
         return strlen(buf);
      }
      o += (size_t)n;
      first = 0;
   }
   return o;
}

int ctl_find(const ctl_map *m, unsigned mask, int except)
{
   int a;
   if (!mask)
      return -1;
   for (a = 0; a < CTL_ACTIONS; a++)
      if (a != except && ctl_available(a) && m->bind[a] == mask)
         return a;
   return -1;
}

int ctl_assign(ctl_map *m, int a, unsigned mask, int *stolen)
{
   int err = ctl_check(a, mask), j;
   if (stolen)
      *stolen = -1;
   if (err != CTL_OK)
      return err;
   j = ctl_find(m, mask, a);
   if (j >= 0)
   {
      m->bind[j] = 0;
      if (stolen)
         *stolen = j;
   }
   m->bind[a] = (uint16_t)mask;
   return CTL_OK;
}

static int repair_in(ctl_map *m, int legacy_swap,
                     void (*note)(void *user, int a, int winner), void *user)
{
   int a, b, changed = 0;

   /* The menu button is a two-valued switch; anything else is corruption,
    * and the safe answer is the mode that never needs a kernel module. */
   if (m->menu_home > CTL_MENU_HOME)
   {
      m->menu_home = CTL_MENU_START_SELECT;
      changed++;
   }
   for (a = 0; a < CTL_ACTIONS; a++)
   {
      if (!ctl_available(a))
      {
         m->bind[a] = 0;
         continue;
      }
      if (ctl_check(a, m->bind[a]) != CTL_OK)
      {
         m->bind[a] = (uint16_t)load_default(a, legacy_swap);
         changed++;
         if (note)
            note(user, a, -1);
      }
   }
   /* Duplicates.  Walking in table order, the first holder of a contested
    * input finds the group; any earlier holder would already have settled it
    * and left a single owner behind. */
   for (a = 0; a < CTL_ACTIONS; a++)
   {
      unsigned mask = m->bind[a];
      int w = -1;
      if (!mask || ctl_find(m, mask, a) < 0)
         continue;
      for (b = 0; b < CTL_ACTIONS && w < 0; b++)
         if (ctl_available(b) && m->bind[b] == mask &&
             mask != load_default(b, legacy_swap))
            w = b;
      if (w < 0)
         w = a;
      for (b = 0; b < CTL_ACTIONS; b++)
         if (b != w && ctl_available(b) && m->bind[b] == mask)
         {
            m->bind[b] = 0;
            changed++;
            if (note)
               note(user, b, w);
         }
   }
   return changed;
}

int ctl_repair(ctl_map *m,
               void (*note)(void *user, int a, int winner), void *user)
{
   return repair_in(m, 0, note, user);
}

static void repair_evt(void *user, int a, int winner)
{
   (void)user;
   FE_EVT_ONLY(a);
   FE_EVT_ONLY(winner);
   if (winner < 0)
      fe_evt("config_bind_invalid key=%s -> default", INFO[a].key);
   else
      fe_evt("config_bind_conflict key=%s lost_to=%s -> NONE", INFO[a].key,
             INFO[winner].key);
}

int ctl_load_ini(ctl_map *m, const char *path)
{
   int a, bad = 0, swap;
   char probe[96];

   /* THE btn_swap MIGRATION.  3.0's Settings "A/B buttons" row stored
    * `btn_swap = 1` for Cross-is-A; the 3.1 candidates kept it as the base
    * a missing bind_a/bind_b resolved to.  The row is gone and the key has
    * no meaning of its own any more, so a config that still depends on it
    * (nonzero, and at least one of the two keys absent) is loaded exactly
    * the way those builds loaded it, and A and B are then marked keep: the
    * next save writes the effective pair, the Controls page shows it, and
    * the key is never consulted again.  With both keys present it is
    * ignored outright -- which is what makes pcfg_save's mirror of it
    * (ctl_legacy_swap) inert for this build. */
   swap = fe_ini_get_int(path, "btn_swap", 0) != 0 &&
          !(fe_ini_get(path, INFO[CTL_GAME_A].key, probe, sizeof(probe)) &&
            fe_ini_get(path, INFO[CTL_GAME_B].key, probe, sizeof(probe)));
   defaults_in(m, swap);
   for (a = 0; a < CTL_ACTIONS; a++)
   {
      char v[96];
      unsigned mask;
      int err;

      if (!ctl_available(a) || !fe_ini_get(path, INFO[a].key, v, sizeof(v)))
         continue;
      m->keep[a] = 1;      /* in the file: always rewritten from now on */
      if (strlen(v) >= sizeof(v) - 1)
         err = -1;         /* fe_ini_get truncated it: not a value we know */
      else if (ctl_parse(v, &mask) != 0)
         err = -1;
      else
         err = ctl_check(a, mask);
      if (err == CTL_OK)
      {
         m->bind[a] = (uint16_t)mask;
         continue;
      }
      bad++;
      FE_EVT_ONLY(err);
      fe_evt("config_bind_invalid key=%s value=\"%s\" reason=%s -> default",
             INFO[a].key, v, err < 0 ? "unparseable" : ctl_error_text(err));
   }
   /* The menu button.  A value we do not know falls back to the default --
    * START+SELECT, which always works -- and is reported like a bad binding;
    * the key stays "in the file" so the next save rewrites it sanely. */
   {
      char v[32];
      int mh;
      if (fe_ini_get(path, CTL_MENU_KEY, v, sizeof(v)))
      {
         m->menu_keep = 1;
         if (strlen(v) < sizeof(v) - 1 && ctl_menu_parse(v, &mh) == 0)
            m->menu_home = (uint8_t)mh;
         else
         {
            bad++;
            fe_evt("config_bind_invalid key=%s value=\"%s\" reason=unparseable"
                   " -> default", CTL_MENU_KEY, v);
         }
      }
   }
   repair_in(m, swap, repair_evt, NULL);
   if (swap)
   {
      char fa[24], fb[24];
      m->keep[CTL_GAME_A] = 1;
      m->keep[CTL_GAME_B] = 1;
      ctl_format(m->bind[CTL_GAME_A], fa, sizeof(fa), NULL, NULL);
      ctl_format(m->bind[CTL_GAME_B], fb, sizeof(fb), NULL, NULL);
      fe_evt("config_migrate key=btn_swap -> bind_a=%s bind_b=%s", fa, fb);
   }
   return bad;
}

int ctl_legacy_swap(const ctl_map *m)
{
   return m->bind[CTL_GAME_A] == CTL_CROSS &&
          m->bind[CTL_GAME_B] == CTL_CIRCLE;
}

int ctl_save_ini(ctl_map *m, const char *path)
{
   int a, rc = 0;
   for (a = 0; a < CTL_ACTIONS; a++)
   {
      char v[64];
      if (!ctl_available(a))
         continue;
      if (!m->keep[a] && m->bind[a] == ctl_default(a))
         continue;
      ctl_format(m->bind[a], v, sizeof(v), NULL, NULL);
      if (fe_ini_set(path, INFO[a].key, v) != 0)
         rc = -1;
      else
         m->keep[a] = 1;
   }
   if (m->menu_keep || m->menu_home != CTL_MENU_START_SELECT)
   {
      if (fe_ini_set(path, CTL_MENU_KEY, ctl_menu_name(m->menu_home)) != 0)
         rc = -1;
      else
         m->menu_keep = 1;
   }
   return rc;
}

uint32_t ctl_game_mask(const ctl_map *m, unsigned pad)
{
   uint32_t r = 0;
   int i;
   for (i = 0; i < CTL_GAME_COUNT; i++)
      if (pad & m->bind[i])
         r |= 1u << INFO[i].retro_id;
   return r;
}

unsigned ctl_shortcuts(const ctl_map *m, unsigned pad, unsigned pad_new)
{
   unsigned f = 0, b;

   b = m->bind[CTL_SC_FF];
   if (!ctl_yields(m, CTL_SC_FF, pad))
   {
      if (ctl_held(pad, b))
         f |= CTL_FIRE_FF_HELD;
      if (ctl_edge(pad, pad_new, b))
         f |= CTL_FIRE_FF_PRESS;
   }
   /* Every action asks ctl_yields(), so CTL_F_YIELD in the table is the one
    * place the longer-chord rule is switched; Video preset has it off (see
    * the table comment). */
   if (ctl_edge(pad, pad_new, m->bind[CTL_SC_VIDEO]) &&
       !ctl_yields(m, CTL_SC_VIDEO, pad))
      f |= CTL_FIRE_VIDEO;
   if (ctl_trigger(pad, pad_new, m->bind[CTL_SC_SAVE]) &&
       !ctl_yields(m, CTL_SC_SAVE, pad))
      f |= CTL_FIRE_SAVE;
   else if (ctl_trigger(pad, pad_new, m->bind[CTL_SC_LOAD]) &&
            !ctl_yields(m, CTL_SC_LOAD, pad))
      f |= CTL_FIRE_LOAD;
   /* The START guard keeps the screenshot off the fixed START+SELECT chord
    * whatever it is bound to (ADR-0069). */
   b = m->bind[CTL_SC_SHOT];
   if (ctl_edge(pad, pad_new, b) && !(pad & CTL_START & ~b) &&
       !ctl_yields(m, CTL_SC_SHOT, pad))
      f |= CTL_FIRE_SHOT;
   if (ctl_trigger(pad, pad_new, m->bind[CTL_SC_PAUSE]) &&
       !ctl_yields(m, CTL_SC_PAUSE, pad))
      f |= CTL_FIRE_PAUSE;
   if (ctl_available(CTL_SC_CATCH) && ctl_held(pad, m->bind[CTL_SC_CATCH]) &&
       !ctl_yields(m, CTL_SC_CATCH, pad))
      f |= CTL_FIRE_CATCH;
   return f;
}

int ctl_yields(const ctl_map *m, int a, unsigned pad)
{
   unsigned ma;
   int s;
   if (!ctl_available(a) || !(INFO[a].flags & CTL_F_YIELD))
      return 0;
   ma = m->bind[a];
   if (!ma)
      return 0;
   for (s = CTL_GAME_COUNT; s < CTL_ACTIONS; s++)
   {
      unsigned ms = m->bind[s];
      if (s == a || !ms || ms == ma || !ctl_available(s))
         continue;
      if ((ms & ma) == ma && (pad & ms) == ms)
         return 1;
   }
   return 0;
}
