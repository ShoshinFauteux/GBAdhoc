# frame_parity.spec -- the per-frame set beyond the hot block (tools/layout/frame_parity.py).
# The hot block (every function the hot order placed, _ftext .. __pin_from_frame_loop) and
# tools/layout/psp_frame_path.txt are in the set without a line here.
#
#   root PATTERN...     runs every frame; its direct callees are in the set too, unless `cold`
#   func PATTERN...     runs every frame (callees not added)
#   cold PATTERN...     a root's callee that does NOT run every frame (menus, session setup and
#                       teardown, error paths, one-shot actions): allowed to differ
#   rigonly PATTERN...  runs per frame only in a rig build; the release never executes it
#   data PATTERN...     a per-frame data object (compared as data, offset mod 8 KiB held)
# Patterns are fnmatch globs on the base name.  A `# reason` ends the line.

root frame_loop                  # the frame loop (pin frame_loop)
root pace_frame net_frame        # frame pacing; the link session's per-frame step
root lat_* ll_* mer_*            # the low-latency present path (docs/LOW-LATENCY.md)
root me_rend_frame me_rend_present me_rend_retire_if_idle   # the ME renderer's CPU side
func plat_video_frame plat_audio_frames audio_ring_note input_fresh
data netpacket_iface             # A's netpacket interface table (layout.lock anchor)

# autopilot: the release's gate is always false (no script can load), so its body never runs
rigonly fe_autopilot_frame fe_autopilot_take_mark fe_autopilot_dump_pending   # behind the gate

# frame_loop's callees that run on an event, not every frame
cold ui_frame ui_open ui_close ui_net_action ui_set_* ui_state_* ui_settings_save ui_*_demo_start ui_home_* ui_wake_shot_arm ui_swallow_pad   # the menu
cold mgift_*                     # Mystery Gift session
cold gblink_begin gblink_win_reset fe_host_gblink_close fe_host_gblink_request_end   # GB link session start/end
cold sleep_q_* sleep_park_hold wake_service wake_snapshot power_request_poll*   # sleep / wake
cold psp_state_load* state_save_slot player_screenshot dump_frame_bmp dump_ge_bmp   # one-shot actions
cold psp_rewind_hud psp_rewind_land* psp_rewind_pool_restart psp_rewind_pwr   # rewind UI / landing
cold home_host_start home_host_tried   # HOME module start
cold net_bringup_rend_off net_teardown rfu_sched_pause   # session setup / teardown
cold fe_host_reset fe_host_state_recover fe_host_option_set_live skip_policy_reapply psp_clock_apply   # reset, recovery, option changes
cold vid_cycle_preset pcfg_display_save   # display preset change (a key press)
cold link_draw_facts link_chip ffh_cap ffh_cap_draw   # on-screen info panels, drawn on request
cold link_wait_osd link_wait_hide   # the wait notice: only while a link holds the game for its partner (no game frame runs), once as the hold ends, and as the menu opens
cold fe_ini_get_int snprintf strrchr psp_local_wallclock   # harness reads, formatting
cold online_psp_enabled
cold sceKernelDelayThread scePowerLock scePowerUnlock sceIoGetstat   # syscall stubs (8 B jumps)
