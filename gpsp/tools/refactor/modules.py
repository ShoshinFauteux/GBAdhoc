#!/usr/bin/env python3
"""modules.py -- PLAN.md §1.2/§1.3 module rules, as data, for split.py (T4) and
globals.py (T9).  Everything here is a transcription of the plan, cited; where
the plan is silent the choice is marked E (an initial assignment the cutlist
review confirms or moves, PLAN §1.2 "Initial assignments ... (E)").

Function -> module (PLAN §1.2):
  1. RANGES: the module table's "From (L:)" column, exactly as
     scratch/crit_cover.awk transcribed it (01067e74 line numbers).  These
     overlap: crit_cover finds 69 functions in 2-3 ranges and 5 in none
     (main, the shell, excluded).  `cover()` reproduces those numbers.
  2. NAMES: the plan's bullet list "Initial assignments for the ambiguous
     items", one entry per name.  Applied first.
  Range rule only for names not in NAMES.  Line ranges are used ONLY when a
  cutlist is generated on 01067e74; on any other tree a cutlist is carried by
  name (split.py gen --carry), never re-derived from these line numbers.

Global -> group (x1 md.py GROUPS, extended for the 23 attributed definitions
the x1 parser missed) -> owner module (PLAN §1.2 "Owns" column, §1.3).
"""
import re

# ---- 1. the module table's ranges (crit_cover.awk, verbatim order) ----
RANGES = [
    ('audio', 700, 1110), ('audio', 8690, 8720),
    ('iothread', 540, 700),
    ('input', 1100, 1810),
    ('power', 430, 526), ('power', 2100, 2600), ('power', 3480, 4188), ('power', 13490, 13720),
    ('display', 2190, 2330), ('display', 3480, 3700),
    ('merend', 1811, 2100), ('merend', 2800, 3116), ('merend', 3256, 3480), ('merend', 4139, 4974),
    ('pacing', 3117, 3255), ('pacing', 6059, 6245), ('pacing', 12960, 13836),
    ('rewind', 11800, 12960),
    ('link', 4975, 9560),
    ('mgift', 9560, 10000),
    ('boost', 6389, 6505),
    ('misc', 10157, 10200), ('misc', 4975, 5900),
    ('inproc', 7290, 7345), ('inproc', 1200, 1500),
    ('teardown', 18068, 18333),
    ('boot', 13880, 15250),
    ('session', 15251, 16367),
    ('loop', 16368, 18067),
    ('harness', 2599, 2850), ('harness', 5600, 5800), ('harness', 6380, 6600), ('harness', 10200, 11800),
]
SHELL_RANGES = [(1, 160), (13837, 18333)]

# ---- 2. PLAN §1.2 "Initial assignments for the ambiguous items" ----
_LINKMISC_TO_HARNESS = ('eboot_crc', 'vram_probe_frame', 'vid_prof_frame')
NAMES = {
    # Unassigned (5)
    'online_build_id': 'link', 'init_paths': 'paths', 'gpsp_net_wait_beat': 'harness',
    'run_testpat': 'harness', 'run_nettest': 'harness',
    # heap census and ROM cache (9, L:1217-1448) -> inproc
    'heap_largest': 'inproc', 'heap_snap': 'inproc', 'heap_snap_same': 'inproc', 'heap_census': 'inproc',
    'boot_ballast_take': 'inproc', 'mem_budget_gba_ok': 'inproc', 'boot_mem_census': 'inproc',
    'rom_cart_blocks': 'inproc', 'rom_cache_select': 'inproc',
    # clock/display/wake (13, L:2216-3696)
    'psp_clock_capture': 'display', 'psp_clock_mode': 'display', 'psp_clock_harness_drop': 'display',
    'psp_clock_actual': 'display', 'psp_clock_apply': 'display', 'psp_display_apply': 'display',
    'wake_snapshot': 'display', 'wake_snapshot_from': 'display', 'me_rend_redraw': 'display',
    'psp_color_preview': 'display',
    'power_cb': 'power', 'standby_note': 'power', 'menu_open': 'power',
    'sleep_park_hold': 'power',
    # pacing/link (2)
    'frame_hist_note': 'pacing', 'frame_hist_evt': 'pacing',
    # boost (8, L:6399-6505)
    'emu_boost_drop': 'boost', 'boost_warden': 'boost', 'emu_boost_open': 'boost', 'emu_boost_close': 'boost',
    'emu_boost_start': 'boost', 'emu_boost_stop': 'boost', 'boost_evt': 'boost',
    'sess_cost_evt': 'harness', 'adhoc_ctl_evt': 'harness',
    # link/inproc (1)
    'inproc_refusal': 'inproc',
    # audio/link (2)
    'audio_nominal_step': 'audio', 'gblink_audio_session': 'audio',
    # park and wake (5, L:13500-13620)
    'loop_park': 'power', 'loop_unpark': 'power', 'browser_park': 'power',
    'wake_overlay_yield': 'power', 'wake_service': 'power',
    # the shell
    'main': 'shell',
}
for _n in _LINKMISC_TO_HARNESS:
    NAMES[_n] = 'harness'
# "link/misc (27, L:4977-5900): every function in that list ... -> misc, except
# eboot_crc, vram_probe_frame and vid_prof_frame -> harness".  The list is the
# set crit_cover reports as {link,misc} or {link,misc,harness}; it is resolved
# by RULE below (not by copying 27 names), and cover() checks the count is 27.
LINKMISC_RULE = 'misc'

MODULE_ORDER = ['shell', 'paths', 'audio', 'iothread', 'input', 'power', 'display', 'merend', 'pacing',
                'rewind', 'link', 'mgift', 'boost', 'misc', 'inproc', 'teardown', 'boot', 'session',
                'loop', 'harness']


def range_hits(line):
    hits = []
    for m, a, b in RANGES:
        if a <= line <= b and m not in hits:
            hits.append(m)
    return hits


def in_shell(line):
    return any(a <= line <= b for a, b in SHELL_RANGES)


def function_module(name, line):
    """-> (module, how).  how: 'name' | 'range' | 'rule:link/misc' | None if
    unresolved (then module is None and `hits` explain why)."""
    if name in NAMES:
        return NAMES[name], 'name'
    hits = range_hits(line)
    if len(hits) == 1:
        return hits[0], 'range'
    if set(hits) in ({'link', 'misc'}, {'link', 'misc', 'harness'}):
        return LINKMISC_RULE, 'rule:link/misc'
    return None, 'none' if not hits else 'multi:' + ','.join(hits)


def cover(funcs):
    """crit_cover.awk reproduced: (none, multi) lists over the raw ranges, with
    main (the shell) excluded as the awk's reader did."""
    none, multi = [], []
    for f in funcs:
        if f['name'] == 'main':
            continue
        h = range_hits(f['start'])
        if not h:
            none.append(f)
        elif len(h) > 1:
            multi.append((f, h))
    return none, multi


# ---- globals: x1 groups (md.py GROUPS) + the 23 the x1 parser missed ----
GROUPS = [
    ('G01', r'^(g_dir_base|p_\w+|g_perf_rig|g_online_boot|s_online_fmt)$'),
    ('G02', r'^(g_running|g_loop_iter|g_loop_phase|g_relaunch|g_exit_assist\w*|g_launch_rom|g_inproc_me_unload|g_wake_answered|g_catch_selftest_addr)$'),
    # T9 fix: g_ffh_*/g_ffc_* (3149-3157, .sbss pins) are fast-forward hold/ceiling state (PLAN §1.2 #8 lists them with G03)
    ('G03', r'^(g_ff_\w+|g_blit_suppress|g_drew|g_ffw_\w+|g_ffh_\w+|g_ffc_\w+)$'),
    ('G04', r'^(g_pwr_\w+|g_cb_thid|g_sq|g_sq_ops|g_ss_\w+|g_rw_pool_suspect)$'),
    ('G05', r'^(g_wake_\w+|g_ui_thumbs|g_last_\w+)$'),
    ('G06', r'^(g_rtc_\w+|gpsp_rtc_wall|g_clock_fake\w*)$'),
    ('G07', r'^(g_stick_\w+|g_pad\w*|g_turbo\w*|g_mgift_consumed|g_home_\w+|g_in_fresh\w*|g_ctrl_cycle)$'),
    ('G08', r'^(g_io_\w+)$'),
    ('G09', r'^(audio_\w+|ring_[rw]|g_audio_\w+|g_ar_\w+|in_rate|g_ring_high_water|g_link_mute|g_gbl_rxq_max)$'),
    # T9 fix: g_vc_work (2247) -> display (PLAN §1.2 #6 names it)
    ('G10', r'^(g_clk_\w+|g_h_\w+|g_vc_work)$'),
    ('G11', r'^(cur_\w+)$'),
    ('G12', r'^(g_me_(video|video_cfg|stage|stage_ready|stage_pending|vf|vdrops|census|vmiss|pw|ph))$'),
    # T9 fix: g_all_clean (3566) is the ME renderer's all-dirty page map (VRAM_DIRTY_PAGES, read by me_rend_redraw) -> G13 (E)
    ('G13', r'^(g_me_rend\w*|g_mer_\w+|g_vm_\w+|g_rom_vm_\w+|g_vhash_\w+|g_flip_early|g_vis_\w+|g_fxs_\w+|g_frame_start_us|g_rw_slot\w*|g_rw_ring|g_rw_flush_posts|g_rwm|g_all_clean)$'),
    # T9 fix: g_dfr*/g_lf_* (3163-3171, 13740-13741; .sbss/.sdata pins) are the late-present/early-flip latency tiers (PLAN §1.2 #8)
    ('G14', r'^(g_ll_\w+|g_lat_\w+|g_vb_\w+|g_bb_\w+|g_lw_\w+|g_vc_target|g_dfr\w*|g_lf_\w+)$'),
    ('G15', r'^(g_rw_\w+|g_rwc|g_rwh)$'),
    ('G16', r'^(g_fps_\w+|vp_\w+|g_vp_\w+|g_fh_\w+|g_pe_\w+|g_telemetry_period|g_shash_\w+|g_dump_\w+|g_yield_\w+|g_gedump_\w+|g_boot_mark\w*|g_boot_t0|g_boot_ballast\w*|g_jit_coh_every|g_emu_thid)$'),
    ('G17', r'^(g_boost_\w+)$'),
    ('G18', r'^(g_net_\w+|g_group_lost_logged|g_session_info|g_save_path_for_backup|g_link_\w+|g_game_\w+|g_gb_title|g_np_start_why)$'),
    ('G19', r'^(g_gba_\w+|g_cable_reserve|g_reload_\w+)$'),
    ('G20', r'^(g_gblink_\w+|g_gbl_\w+|g_gw_\w+|g_sessmem_\w+|g_script_path)$'),
    ('G21', r'^(g_rfu_\w+)$'),
    ('G22', r'^(g_silent_\w+|g_mg_\w+)$'),
    ('G23', r'^(g_shot_\w+)$'),
    ('G24', r'^(g_autoload_\w+)$'),
    ('G25', r'^(g_stall_\w+|g_hb_\w+|g_drs_\w+|g_drt_\w+|g_lps_\w+|g_log_input|g_fail_probe|g_watch_\w+|g_rig_prev_uncollected|g_uis\w*|g_turbo_lock_sim)$'),
]
GROUP_NAMES = {
    'G00': 'imports (extern decls)', 'G01': 'paths/boot-config', 'G02': 'lifecycle/exit', 'G03': 'fast-forward',
    'G04': 'power/sleep/standby', 'G05': 'wake frame/overlay', 'G06': 'rtc/clock', 'G07': 'input/pad/turbo',
    'G08': 'io writer thread', 'G09': 'audio', 'G10': 'CPU clock/display', 'G11': 'video frame handoff',
    'G12': 'ME video offload (legacy me_video)', 'G13': 'ME renderer', 'G14': 'low-latency pacing', 'G15': 'rewind',
    'G16': 'telemetry/profiling', 'G17': 'emu boost', 'G18': 'net/link session', 'G19': 'GBA cable', 'G20': 'GB link',
    'G21': 'RFU', 'G22': 'Mystery Gift / silent', 'G23': 'screenshot', 'G24': 'states',
    'G25': 'harness: stall/drprof/watch/uiscript'}

# group -> owner module, PLAN §1.2 "Owns" column
GROUP_MODULE = {
    'G01': 'paths',      # #20
    'G02': 'power',      # #5 owns g_running (G02); the rest of G02 is not named by the plan -> power (E), except NAME_OWNER
    'G03': 'pacing',     # #8 "G14 and G03"
    'G04': 'power', 'G05': 'power',   # #5
    'G06': 'misc',       # #13
    'G07': 'input',      # #4
    'G08': 'iothread',   # #3
    'G09': 'audio',      # #2
    'G10': 'display',    # #6
    'G11': 'merend',     # §1.3: "G11 ... sits in merend's range at L:1813"
    'G12': 'merend', 'G13': 'merend',  # #7
    'G14': 'pacing',     # #8
    'G15': 'rewind',     # #9
    'G16': 'harness',    # #19 "G16/G25"
    'G17': 'boost',      # #12
    'G18': 'link', 'G19': 'link', 'G20': 'link', 'G21': 'link', 'G22': 'link',   # #10 "G18-G22"
    'G23': 'misc', 'G24': 'misc',   # #13
    'G25': 'harness',
}
# name-level owners the plan states explicitly
NAME_OWNER = {
    'g_relaunch': 'inproc',      # #14 "Owns g_relaunch"
    'g_online_boot': 'paths',    # #20 "Its definition stays here and link only reads it"
}
# #11 mgift owns "G21 g_mg_*, parcel": the plan's G21 is x1's G22 (Mystery Gift / silent);
# g_mg_* -> mgift, g_silent_* stays with link (#10 "g_silent_*").  parcel is a function-local static.
NAME_RULES = [(r'^g_mg_\w+$', 'mgift')]


def group_of(v):
    if v.get('extern'):
        return 'G00'
    for g, rx in GROUPS:
        if re.match(rx, v['name']):
            return g
    return 'G99'


def owner_of(v, group=None):
    """-> (module, how) or (None, 'unassigned')."""
    g = group or group_of(v)
    if g == 'G00':
        return None, 'extern'
    if v['name'] in NAME_OWNER:
        return NAME_OWNER[v['name']], 'name'
    for rx, m in NAME_RULES:
        if re.match(rx, v['name']):
            return m, 'rule'
    if g in GROUP_MODULE:
        return GROUP_MODULE[g], 'group'
    return None, 'unassigned'
