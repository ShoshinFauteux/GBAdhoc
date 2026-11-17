#define BASE_DIR_DEFAULT "ms0:/PSP/GAME/gpsp-adhoc"
char g_dir_base[128] = BASE_DIR_DEFAULT;     /* also used by ui_psp */
static char p_rom_dir[144], p_log_dir[144], p_log_path[160];
#if !HARNESS_FOLD
static char p_harness[160], p_config[160], p_variant[160];
#else
static char p_config[160], p_variant[160];   /* a player build has no harness path (C2-5) */
#endif
static char p_legacy_ap[160];
static int g_perf_rig;

/* THE HARNESS CONTROL CHANNEL — deliberately NOT a user-facing filename
 * (ADR-0036).  This file can auto-host, auto-join, skip the ROM browser,
 * force fast-forward and pin performance knobs; a stale copy on a memory
 * stick silently brought a wireless session up during a SOLO benchmark, and
 * had twice before skipped the browser or auto-hosted unexpectedly.
 *
 * It used to be called `autopilot.ini`, which sits in the same directory as
 * `config.ini` and `variant.ini` and looks exactly like something a user is
 * meant to edit.  That was the design error: an internal channel wearing a
 * user-config name.  The leading dot and the `gpsp-` prefix now say
 * "internal" to a human packaging a kit, and nobody creates this by accident.
 *
 * `variant.ini` is deliberately NOT renamed — that one is user-chosen.
 *
 * Rig builds only (C2-5): a player build folds every read to its default
 * (fe_priv.h HARNESS_FOLD) and has no HARNESS_INI to name. */
#if !HARNESS_FOLD
#define HARNESS_INI p_harness
#endif
/* Read-only: checked so a leftover can be REPORTED, never obeyed, never
 * deleted (it is the user's file, even when it is in the way). */
#define LEGACY_AUTOPILOT_INI p_legacy_ap
#define ROM_DIR     p_rom_dir
#define LOG_DIR     p_log_dir
#define LOG_PATH    p_log_path
#define CONFIG_INI  p_config
#define VARIANT_INI p_variant

/* NOTE: `GPSP_NO_TELEMETRY` is derived from `GPSP_PLAYABLE` in fe_evt.c, NOT
 * here.  It was here first, and it did nothing: `fe_evt.c` is a separate
 * translation unit and never saw this file's #define, so the playable build
 * linked with telemetry fully present while every token-presence check passed.
 * Caught by `psp-nm -u frontend-common/fe_evt.o` still listing `fopen` /
 * `fwrite` / `fflush` / `vsnprintf` — a BEHAVIOURAL check, which is the only
 * kind that would have caught it. */
/* PLAY_EMU_PRIO .. PLAY_ME_VIDEO (14 macros): their definitions are in
 * fe/play_defaults.h. */

/* LOW LATENCY test builds (docs/LOW-LATENCY.md): the default of the
 * `input_fresh` pad re-peek, set by `INPUT_FRESH=1 tools/build.sh ...`
 * (-DPLAY_INPUT_FRESH=1) so a playable build, which cannot read the harness
 * ini, can carry it.  0 = the shipping behaviour; a harness ini still
 * overrides it either way. */
#ifndef PLAY_INPUT_FRESH
#define PLAY_INPUT_FRESH     0
#endif

static void init_paths(int argc, char *argv[])
{
   if (argc > 0 && argv && argv[0])
   {
      const char *slash = strrchr(argv[0], '/');
      if (slash && (size_t)(slash - argv[0]) < sizeof(g_dir_base))
      {
         memcpy(g_dir_base, argv[0], (size_t)(slash - argv[0]));
         g_dir_base[slash - argv[0]] = '\0';
      }
   }
   snprintf(p_rom_dir, sizeof(p_rom_dir), "%s/roms", g_dir_base);
   snprintf(p_log_dir, sizeof(p_log_dir), "%s/log", g_dir_base);
   snprintf(p_log_path, sizeof(p_log_path), "%s/log/frontend.log", g_dir_base);
   /* ADR-0067 (amended by code-health C2-3/C2-5): a player build has no harness
    * path.  Its reads are folded to their defaults at compile time
    * (fe_priv.h HARNESS_FOLD), the value the never-opened path
    * `.playable-no-harness/x` gave them at run time before; the source
    * expression is still the one rig builds compile, and
    * tools/refactor/harness_keys.py checks that the defaults agree. */
#if !HARNESS_FOLD
   snprintf(p_harness, sizeof(p_harness), "%s/.gpsp-harness.ini", g_dir_base);
#endif
   snprintf(p_legacy_ap, sizeof(p_legacy_ap), "%s/autopilot.ini", g_dir_base);
   snprintf(p_config, sizeof(p_config), "%s/config.ini", g_dir_base);
   snprintf(p_variant, sizeof(p_variant), "%s/variant.ini", g_dir_base); /* @lab */ LAB_HOOK(lab_boot(g_dir_base, p_harness, sizeof(p_harness), p_rom_dir, sizeof(p_rom_dir), p_log_dir, sizeof(p_log_dir), p_log_path, sizeof(p_log_path));)
}

